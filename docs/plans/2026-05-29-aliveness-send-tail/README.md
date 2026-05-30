---
title: Aliveness Send-Tail — Wiring Intrinsic Goals to the Proposer (default-off)
description: Connect A3 intrinsic-originated goals to the init_proposer send path, so "presence" produces observable behavior instead of audit logs. Building blocks already exist; this is the daemon orchestration. Outbound stays OFF until the operator flips it.
status: ready-to-execute
created: 2026-05-29
owner: seth
risk: HIGH (daemon.c; the step that makes h-uman initiate messages to real contacts)
---

# Aliveness Send-Tail — Wiring Plan

## Why this is a plan, not a landed change

The send-tail is the code that makes h-uman *initiate messages to real
contacts*. Three facts make "write it carefully in a focused session" the right
call rather than landing it under a long multi-item push:

1. It lives in `daemon.c` (~14.6k lines, the most central file).
2. It cannot be end-to-end verified without enabling outbound — so the usual
   "/verify by running it" gate is itself the authorization-gated action.
3. The decision logic it needs **already exists and is tested** — so there is
   no safe, non-speculative unit to build ahead of the wiring (building a
   no-caller predicate would violate KISS/YAGNI).

The operator (Seth) authorized *building it default-off*; this plan is exactly
that, scoped so the live flip remains a separate, explicit step.

## Confirmed building blocks (verified 2026-05-29)

| Piece | Location | Role |
|---|---|---|
| Intrinsic tick → STARTED + goal | `hu_intrinsic_run_tick` → `hu_intrinsic_tick_result_t` (`intrinsic_drive.h:95`) | originates a self-driven goal |
| Share gate (pure) | `hu_intrinsic_may_share(confidence)` (`intrinsic_drive.h:77`) | "no egress that bypasses init_proposer"; bar = `HU_INTRINSIC_SHARE_MIN_CONFIDENCE` 0.85 |
| Proposer governor (check-only) | `hu_init_proposer_governor_check_only(...)` (`init_proposer.h:73`) | silence-biased gate stack, no send |
| Proposer LLM tick (draft + decide) | `hu_init_proposer_tick_with_provider(...)` (`init_proposer.h:201`) | returns `{should_propose, draft, reason}` |
| Existing tick site (logs intent only) | `daemon.c:3347-3367` | where the wiring attaches |
| Config gate (default false) | `cfg.intrinsic.enabled` (`intrinsic_runtime_cfg`) | the live switch |

## The wiring (at `daemon.c:3365`, replacing the "logs intent only" comment)

```c
if (hu_ir.outcome == HU_INTRINSIC_TICK_STARTED) {
    /* A3 originated a goal. Escalate to the proposer ONLY behind the same
     * silence-biased gate stack used for all proactive outbound. */
    hu_init_proposer_propose_t prop = {0};
    hu_init_proposer_result_t pr = hu_init_proposer_tick_with_provider(
        &config->initiative, agent, hu_now, /*provider*/..., &prop);
    if (pr == HU_INIT_RESULT_PROPOSED && prop.should_propose &&
        hu_intrinsic_may_share(prop.confidence)) {
        /* route prop.draft through the SAME scheduled-message dispatch the
         * follow-up + prosocial routines already use (validator chain +
         * complexity-vary + casing), addressed to the chosen contact. */
        hu_daemon_enqueue_proactive(agent, config, prop.draft, /*contact*/...);
    } else {
        /* gate said no -> audit-log the skip reason; no send. */
    }
}
```

Open implementation questions to resolve IN the focused session (do not guess):
- **Recipient selection.** An intrinsic goal isn't addressed to anyone. Who
  receives a self-originated share? Likely the self-chat (Seth) only, at least
  for v1 — never an arbitrary contact. Confirm `init_proposer`'s existing
  recipient policy and reuse it; do NOT invent a new recipient path.
- **Provider plumbing.** `hu_init_proposer_tick_with_provider` needs a provider;
  reuse the daemon's `propose_model` (`initiative.propose_model` =
  gemini-3.5-flash in live config) the same way the existing init_proposer tick
  does.
- **Does `hu_init_proposer_propose_t` carry `confidence`?** Verify the field
  exists; if the proposer returns no confidence, `hu_intrinsic_may_share` needs
  its input sourced (e.g. map should_propose→1.0/0.0, or add confidence to the
  proposer result). Resolve before wiring.

## Tests (before any live flip)

1. `HU_IS_TEST`-guarded: with `cfg.intrinsic.enabled=false`, the tick site is a
   no-op + the one-shot disabled-log fires (silent-config-gated-subsystems rule).
2. With enabled=true but proposer governor → NEGATIVE, assert **no enqueue**.
3. With enabled=true + proposer PROPOSED + confidence ≥ 0.85, assert exactly one
   enqueue with the draft, addressed to the v1 recipient (self-chat).
4. confidence < 0.85 → no enqueue (the `hu_intrinsic_may_share` bar).

## The live-flip step (operator-gated — NOT part of building this)

Only after the above is merged + tests green:
1. Set `intrinsic.enabled=true` in `~/.human/config.json`.
2. Watch `~/.human/logs/` for the first originated+proposed share for ≥24h with
   recipient pinned to self-chat.
3. Only after observing sane behavior, widen recipient policy (separate review).

## A4 interoception (the other aliveness leg) — separate, larger

`A4 interoception` has **no source file** (verified 2026-05-29). It is a new
module (`src/cognition/interoception.c`), not a wiring task, and is out of scope
for this send-tail plan. Tracked as its own future spec: a self-model of
"how am I doing" (load, recent-success, drive) that gates tone ("upbeat" only
when interoceptive state supports it).

## Related
- `.claude/rules/silent-config-gated-subsystems.md` — the disabled-log discipline
- `~/.claude/rules/security-predicate-extraction.md` — why the share decision is
  already a pure predicate (`hu_intrinsic_may_share`)
- memory: `aliveness_levers_state` — A1 wired, A2 live, A3 propose-only, A4 unbuilt
