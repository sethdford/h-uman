---
title: Director v2 — intent instead of length, learned tapbacks (HU_DIRECTOR_V2)
created: 2026-10-02
status: operator-facing
---

# Director v2

The scene director runs before every reactive reply. It decides whether Seth
texts, reacts with a tapback, or stays silent, and it writes a one-line
`direction` that the reply model follows. v1 lives in
`src/daemon/daemon_director.c`. v2 lives in `src/daemon/director_v2.c` and
`src/daemon/director_tapback.c`, and the daemon calls it at one site
(`hu_director_v2_decide`, `src/daemon.c`).

## Why

The prod log from 2026-09-17 to 2026-10-01 holds 456 director decisions.

- **335 of the 456 (73.5%)** carried a length or deflection cue. Examples are
  "one line", "a few words", "keep it light", "non-committal" and "don't
  over-explain". This count comes from `hu_director_directive_flags`, which
  is the measurement helper below.
- **108 of the 456 (23.7%)** were tapback-only.
- Three real cases:
  - A parent shared covid news and asked how Seth was doing. The direction
    was `Acknowledge the sickness briefly, keep it low-pressure, one line`.
  - "Walk me through the thanksgiving plan" got `Keep it light and
    non-committal`, and the reply dodged.
  - "Seth your AI is messed up?" got `Laugh it off` and a "Liked" tapback.

v1 sees 5 messages. It has no contact context, and its prompt says "BREVITY
IS THE DEFAULT".

## What v2 changes

1. **It directs intent, never length.** The direction has three parts: what
   they are really saying, the move (engage fully, ask a follow-up, share
   something of his own, or just react), and what to draw on from the shared
   history.
   - The prompt forbids line, word and sentence counts. Length belongs to the
     length policy and learned style.
   - It forbids dodging a real question or a walk-me-through request.
   - A test pins that the prompt contains no length cue.
2. **Tapback-only comes from what Seth actually does, not from a word list.**
   The details are below.
3. **It sees more context, cheaply.**
   - The last 12 messages, labelled Seth/Them. They are a separate channel
     read, so the rest of the turn still sees its usual 10.
   - A `Contact:` line with the relationship and Dunbar layer, never a name.
   - The learned "How Seth reacts" line.
   - Budgets are enforced by tests: the system prompt (≈2.1 KB, 2.5 KB with
     the forms block) is capped at 2,560 bytes. The per-turn context is also
     capped at 2,560 bytes: 130 bytes per message, 360 for the new message,
     300 for the facts line.

v2 uses the same provider and model as v1 (`g_classify_provider`). When #587
moves the director to the local model under `privacy.local_only`, v2 follows
it.

## Tapback-only: learned data, no rules

The source is the learned-style profile,
`<persona dir>/<persona>.learned-style.json` (schema `learned-style/v1`, from
the learner in #584). v2 reads these **optional** fields from a contact's
`shape:<x>` bucket, the contact's `overall`, or `global`, in that order:

| Field | Meaning |
|---|---|
| `tapback_only_rate` | how often Seth replied with only a tapback, in that cell |
| `tapback_types` | his mix of reactions, e.g. `{"heart":0.7,"haha":0.3}` |
| `tapback_disengage_rate`, `tapback_disengage_n` | how often, after his tapback-only replies, they went quiet or pushed for a real answer |

The shape is the learner's bucket rule: `question` if the text has a `?`,
`story` if it is long or has several sentences, `casual` otherwise.

**What the model sees.** One plain-fact line, for example:

> How Seth reacts, measured from his own texts: with them, when they ask
> something he replies with only a reaction 3% of the time (n=40); his
> reactions: heart 70%, haha 30%.

The model decides.

**The post-check is a learned threshold.**

- **What it is.** The cutoff is the lower quartile (nearest rank) of Seth's
  own `tapback_only_rate` values. They are taken over every contact cell: each
  contact's `shape:*` buckets and its `overall`.
- **When it acts.** It turns a tapback-only decision into text only when the
  rate for this contact and shape is at or below that cutoff. In other words,
  only where his own history says he almost never does it.
- **When there is no data.** If no rate is found, or there are fewer than 4
  cells, there is no override and the model's choice stands
  (`tapback_src=nodata`).
- **Why the lower quartile.** It is the conventional "low" boundary of a
  distribution, and it moves with his data. The constants are only the
  quartile rank and the 4-cell minimum below which a quartile means nothing.

**What exists today.**

- learned-style/v1 has no reaction fields yet. Until the learner writes them,
  every turn logs `tapback_src=nodata`, and v2 tapbacks are the model's
  choice, informed only by the prompt.
- No disengagement data exists inside h-uman. `outbound_sends.kind` admits only
  `text`, `media` and `reply`, so no tapback is recorded. `reaction_lookup`
  holds their reactions to us, and `proactive_decisions` is proactive-only.
  The learner should derive `tapback_disengage_rate` from chat.db: Seth's
  tapbacks (`associated_message_type` 2000–2005, `is_from_me`), then what the
  contact sent next.
- The reader is local (`hu_tapback_profile_load`) because #586
  (`hu_learned_style_lookup`) is not merged. When it merges, move the shape
  rule and the file read onto it.

## The gate

`HU_DIRECTOR_V2=off|shadow|live`, default **off**, parsed by
`hu_gate_mode_from_env`.

- **off**: `hu_director_v2_decide` is exactly `hu_daemon_director_call`.
  There is no extra read, call or log. A test pins that the result is
  byte-identical.
- **shadow**: v1 decides. v2 is computed alongside, with a second director
  call per turn, and one aggregate line is logged (counts and enums only, no
  text, no handle):

      [director_v2 shadow] v1_action=text v2_action=text v1_brevity=1 v2_brevity=0 tapback_overridden=0 tapback_src=nodata shape=question v2_bytes=2310

- **live**: v2 decides. If the v2 call fails, v1 decides
  (`fallback_v1=1`). The log line is
  `[director_v2 live] v2_action=… v2_brevity=… tapback_overridden=… tapback_src=… shape=… v2_bytes=… fallback_v1=0|1`.

**Latency cost in shadow:** one more director round trip per reactive turn
(v2 runs before v1, sequentially). It has not been measured. Expect roughly
v1's own director time again, readable from the gap between the
`[director_v2 shadow]` line and the `meta:` line. Prod GPU and quota budget
is acceptable for now. LIVE has the cost of one call, as today.

## Promotion: SHADOW → LIVE

Run SHADOW for at least 7 days, then measure all three:

**(a) Shadow log.**

```bash
grep -h '\[director_v2 shadow\]' ~/.human/logs/service-loop-error.log | awk '
  {for(i=1;i<=NF;i++){split($i,kv,"="); f[kv[1]]=kv[2]}
   n++; v1b+=f["v1_brevity"]; v2b+=f["v2_brevity"]
   if (f["shape"]!="casual") {qs++; if (f["v2_action"]=="tapback") qt++}
   if (f["tapback_src"]!="nodata") data++}
  END{printf "n=%d v1_brevity=%.1f%% v2_brevity=%.1f%% qs_tapback=%d/%d learned_data=%d/%d\n",
      n,100*v1b/n,100*v2b/n,qt,qs,data,n}'
```

Pass when all of these hold:

- `v1_brevity` is near the 73% baseline. This confirms the helper reads the
  same thing.
- `v2_brevity` is **≤ 25%**.
- On question and story turns, v2's tapback share is no higher than Seth's own
  learned rate for those shapes, and it is 0 wherever `tapback_src=learned`
  fired.
- A precondition: `learned_data` covers most turns. This means the learner
  writes `tapback_only_rate`. Without it, the tapback half is unmeasured, and
  only the length half can be promoted.

**(b) Replay A/B.** Use `human replay` from the sibling PR
`feat/replay-harness`. Run the same inbound threads through v1 and v2. Reply
depth must rise (median reply bytes, and the share of replies that answer the
question asked). The fragment rate must not rise.

**(c) Blind gate.** Run the human/synthetic blind gate
(`scripts/blind_ab_gate.py`) with `HU_DIRECTOR_V2=live` on the candidate arm.
Detection must hold or fall.

**Gated on:** do not flip to LIVE without all three.

## Rollback

Remove `HU_DIRECTOR_V2` from the service-loop plist environment, or set it to
`off`. Then reinstall with `scripts/install-human-daemon.sh`. v2 persists
nothing, so there is nothing to undo.
