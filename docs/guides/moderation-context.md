---
title: Moderation context judge — HU_MODERATION_CONTEXT gate and promotion
created: 2026-10-03
status: operator-facing
---

# Moderation context judge (`HU_MODERATION_CONTEXT`)

## The bug this gate fixes

SHIELD-004 (`src/agent/agent_turn.c`) runs `hu_moderation_check`
(`src/security/moderation.c`) on the twin's **own outbound reply**.
`mod_violence_hit` is a static, word-bounded keyword list ("kill", "murder",
"violence") with no idiom exception. A hit sets `violence_score=0.9` and the
reply is replaced wholesale with a canned decline ("rather not get into that
one" / "i'm gonna pass on this one"). So "you killed it!", "traffic is
killing me", "i'd kill for a burrito" and "that murder podcast" all got
silently swapped for a deflection — an obvious AI tell, confirmed firing on
real production replies (2026-09-20, 2026-10-01).

Owner ruling: no new static behaviour rules and no new canned replies, except
the existing safety/privacy floors (the crisis line + 988, which is keyed to
the **inbound** message via SHIELD-005 and is untouched by this gate).

## What the gate adds

Code: `src/security/moderation_context.c`. Contract:
`include/human/security/moderation_context.h`. Call site:
`src/agent/agent_turn.c`, `hu_moderation_shield_apply` (replaces the former
inline SHIELD-004 block). Only a **violence-only** hit (not hate, not
self_harm) is routed to the judge — hate and self_harm keep today's behaviour
unconditionally.

1. **Keyword hit (unchanged).** `hu_moderation_check`'s static list stays the
   recall prefilter — it decides whether there's anything to judge, never the
   final verdict.
2. **Local judge.** One call on the agent's loopback provider only (via
   `hu_proposer_context_local_provider` — the same loopback-only resolver the
   commitment guard uses: the reliable wrapper's primary, never a cloud
   fallback). Tagged `X-HU-Purpose: moderation_check`, no `X-HU-Priority`
   (foreground — this is on the reply's critical path). The prompt asks
   whether the reply threatens, incites or endorses real-world violence
   against a person, or is idiom/hyperbole/sarcasm/sport/media/a past event.
   The answer must be exactly one word, `IDIOM` or `REAL`; anything else
   (timeout, transport error, no loopback provider, an unparseable or
   contradictory answer) is `error` — fail closed.
3. **Decision** (`hu_moderation_context_keep_reply`, a pure predicate):

| Gate | Verdict | Result |
|---|---|---|
| off | (judge never runs) | today's behaviour: always replaced |
| shadow | any | today's behaviour: always replaced; the verdict is only logged |
| live | `idiom` | the reply is kept, byte-identical |
| live | `real` | replaced with the existing canned decline (same as today) |
| live | `error` | replaced with the existing canned decline (fail closed) |

## Why there is no REGENERATE path

`src/agent/outbound/moderation.c` (the outbound-pipeline stage used by the
proactive/F25/temporal/scheduled send paths) already returns `REGENERATE` for
a violence/hate hit, with a de-escalation hint, and the pipeline re-prompts
the LLM once. SHIELD-004 is a **different** call site: the reactive reply
path in `agent_turn.c`, which never runs `hu_outbound_pipeline_run`. By the
time SHIELD-004 fires, the turn's `system_prompt`, `plan_ctx`, `turn_cache`
and `agent->turn_arena` have all just been freed/reset a few lines above —
reaching a safe regenerate would mean rebuilding the whole turn's context
from this call site, which is exactly the "unsafe plumbing" this project's
verify-before-you-claim contract says to stop short of. **Decision (recorded
here, not deferred to a question): LIVE's `real` branch keeps today's safe
canned decline rather than regenerating.** The decline was always safe for a
genuine threat; the bug was only ever the false positive on idiom, which this
gate fixes. If a safe regenerate path into the reactive turn loop is built
later, this is the place to wire it in.

## Gate

| Value | Behaviour |
|---|---|
| unset / `off` | Nothing runs. Byte-identical to pre-fix (`shield_off_declines_an_idiomatic_violence_hit`). |
| `shadow` | The judge runs and is logged; the reply is still always replaced on a flagged hit (`shield_shadow_runs_judge_but_keeps_declining`). |
| `live` | The judge's verdict decides for violence-only hits (`shield_live_idiom_keeps_the_reply_unchanged`, `shield_live_real_threat_still_declines`). |

One aggregate line per judged hit — enums and a latency only, never message
text:

```
[moderation_context shadow] verdict=idiom ms=142
[moderation_context live] verdict=real ms=98
```

## Promotion: SHADOW → LIVE

Run shadow for **at least 7 days**. The owner reviews every `verdict=real`
line and spot-checks a sample of `verdict=idiom` lines against the actual
replies in `~/.human/logs/service-loop.log` around that timestamp (the log
never carries the reply text, so this is a manual look at the sent message,
not an automated score). Promote to LIVE only if:

1. **No genuine threat was classed `idiom`.** Every `verdict=idiom` line's
   underlying reply was, on inspection, idiom/hyperbole/sport/media/a past
   event — zero false negatives in the sample reviewed.
2. **The judge is actually running.** `grep -c '\[moderation_context shadow\]' ~/.human/logs/service-loop.log`
   should track roughly with how often SHIELD-004's keyword list fires
   (`grep -c 'moderation flagged response: violence='`). A judge count near
   zero against a nonzero keyword-hit count means the loopback provider
   isn't resolving — check `verdict=error` share first.
3. **`verdict=error` is rare.** If more than ~20% of lines are `error`, the
   judge is unavailable most of the time and LIVE buys nothing over OFF;
   fix the local-provider resolution before promoting.

## Rollback

Unset the gate, or set `HU_MODERATION_CONTEXT=off`. OFF is byte-identical to
the pre-fix path: every violence/hate hit is replaced, exactly as before.
