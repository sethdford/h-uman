# src/behavior/ — Behavior layer

Central, vtable-friendly relational policy that composes existing persona,
memory, voice, and security primitives. Implements workstreams B1–B6 of
[`docs/plans/2026-05-10-behavior-v1-execution-plan.md`](../../docs/plans/2026-05-10-behavior-v1-execution-plan.md).

## Modules

| File | Workstream | Role |
| --- | --- | --- |
| `policy.c` | B1 | `hu_behavior_policy_t` vtable + default heuristic policy |
| `dialog_act.c` | B2 | dialog act classification + other-initiated repair detection |
| `affect.c` | B3 | continuous valence/arousal/dominance affect (text baseline) |
| `change.c` | B4 | BCT + Fogg + JITAI behavior-change selector |
| `safety.c` | B5 | composes SHIELD-001 + vulnerability + attachment trajectory |

## Rules

- **No duplication.** This layer never re-implements persona logic, mood
  state, voice turn-taking, or SHIELD-001 detection. It composes them.
- **Pure functions where possible.** B2/B3/B4/B5/B6 are I/O free; B1 is the
  only module with a vtable, and the default policy needs no allocation.
- **Hard safety rules.** B4 gates persuasion on `autonomy_risk` and
  consent flags. B5 escalates dependency rather than reinforcing it. B1
  always lets safety overrides win over warmth.
- **Coexists with existing names.** `hu_relational_act_t`, `hu_dialog_act_t`,
  `hu_affect_state_t`, `hu_behavior_change_t`, `hu_behavior_safety_t`,
  `hu_persona_eval_t` — all distinct from `hu_turn_signal_t` /
  `hu_turn_action_t` (voice duplex) and `hu_affect_mirror_*` (persona).

## Tests

- `tests/test_behavior_policy.c`
- `tests/test_behavior_dialog_act.c`
- `tests/test_behavior_affect.c`
- `tests/test_behavior_change.c`
- `tests/test_behavior_safety.c`
- `tests/test_persona_eval.c`

Run all of them with:

```bash
./build/human_tests --suite=behavior
./build/human_tests --filter=persona_eval
```
