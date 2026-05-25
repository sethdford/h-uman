---
title: "Phase 5 Eval Gate — Honest Status (2026-05-18 audit)"
created: 2026-05-18
status: closed
parent: docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md
related:
  - docs/plans/2026-05-18-adapter-routing-decision.md
  - ~/.claude/rules/audit-verify-before-allege.md
trigger: parent plan reads `status: design` (all 75+ task checkboxes unchecked) but src/eval/ ships 14 .c files / 4,776 LOC and tests/test_eval_*.c ships 11 test files. The plan-doc field is stale and should be corrected.
last_audit: 2026-05-25
---

# Phase 5 Eval Gate — Honest Status

## Why this doc exists

The parent plan `docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md`
has `status: design` and every Task checkbox unchecked. The actual code in
the tree contradicts that field:

| Source path | LOC | Tests |
|---|---|---|
| `src/eval/eval_gate.c` | 102 | `tests/test_eval_gate.c` |
| `src/eval/bootstrap_ci.c` | 202 | `tests/test_bootstrap_ci.c` |
| `src/eval/persona_fidelity.c` | 299 | `tests/test_persona_fidelity.c` + `_cross.c` + `_judge.c` + `_validator.c` |
| `src/eval/consistency.c` | 239 | `tests/test_consistency.c` |
| `src/eval/leaderboard.c` | 183 | `tests/test_leaderboard.c` |
| `src/eval/eval_judge_external.c` | 171 | (judges) |
| `src/eval/apple_fm_client.c` | 28 | `tests/test_apple_fm_client.c` |
| `src/eval/gemini_nano_client.c` | 28 | `tests/test_gemini_nano_client.c` |
| `src/eval/competitive_harness.c` | 164 | `tests/test_competitive_harness.c` |
| `src/eval/cli_eval.c` | 386 | (CLI subcommands `human eval competitive/leaderboard/gate`) |
| `src/eval/external_judge_fixture.c` | 101 | (test fixture) |
| `src/eval/stock_baseline.c` | 87 | (baseline) |
| `src/eval/persona_rollout.c` | 235 | (rollout) |
| `src/eval/longmemeval.c` | 220 | (longmem) |
| `src/eval/turing_adversarial.c` | 475 | (Turing) |
| `src/eval/turing_score.c` | 1856 | (Turing) |
| **Total** | **4,776** | **11+ test files** |

CLI surface is wired via `cli_commands.c:1220 cmd_eval` dispatching to
`hu_eval_cli_competitive`, `hu_eval_cli_leaderboard`, `hu_eval_cli_gate`.

This is **not** a `status: design` artifact. The implementation has
substantially landed.

## What's actually remaining

The gap is **not implementation**. It's:

1. **No real eval has ever been RUN.** The most recent gate artifact
   at `~/.human/proofs/2026-05-17-dpo-step-90772/gate_decision.json`
   reads `"source": "hu_eval_gate_decide_from_arrays_for_test"` with
   all real runners (`mt_bench`, `ifeval`, `reward`) **NULL** and
   `persona_ci_lower/upper = 0.500000` (test-mode flat). The harness
   compiles and the unit tests pass, but no production eval has been
   triggered against a real adapter.

2. **The parent plan's task checkboxes are inaccurate.** Every Task
   shows `[ ]` despite the corresponding code shipping. Auditing
   which Task corresponds to which committed line range is a doc-only
   chore but should happen before the next planning cycle uses this
   doc as input.

3. **`tail -30` of the parent plan** lists out-of-scope deferrals
   (`❌ Web dashboard visualisation`, `❌ Gate-config UI in onboard`,
   `❌ 5th axis: timing match`). These remain valid v1.5 items.

## The single highest-leverage next step

**Run the eval gate end-to-end against the currently-loaded
`persona-v8` adapter.** Per `cli_eval.c:293 hu_eval_cli_gate`,
this is one CLI command (signature TBD until I read the CLI; see
the verification script in the sibling doc). Outcome:

- A real `gate_decision.json` artifact in `~/.human/proofs/` with
  non-test source and non-flat CI bounds.
- A measurable answer to "is persona-v8 better than stock Gemma
  at texting like Seth?" — the original question that opened this
  audit chain.
- Empirical input for the `2026-05-18-adapter-routing-decision.md`
  recommendation (currently based on architecture-only reasoning).

## What does NOT need a new spec

The parent plan covers the implementation work. The action here
is to:

1. Update the parent plan's `status:` field to `substantially-implemented`.
2. Cross out Task checkboxes that correspond to shipped code.
3. Tag the remaining checkboxes as "in-flight" or "deferred to v1.5"
   per the actual repo state.

This is a doc-correction pass, not new architecture.

## The verification command (once daemon binary is rebuilt)

```bash
# After build completes — restart daemon + verify reaction_collection accepted
launchctl bootout gui/$(id -u)/ai.human.intelligence-cycle 2>&1 || true
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.intelligence-cycle.plist

# Wait 5s for startup
sleep 5

# Confirm config is now ACCEPTED (no 'unknown key' line)
grep "unknown key.*reaction_collection" ~/.human/logs/intelligence-cycle-error.log
# Expected: NO MATCH (empty output). Previous build emitted this on every startup.

# Confirm poll loop is running
grep "reaction.*poll\|tapback" ~/.human/logs/intelligence-cycle-error.log | tail -5

# Trigger first real eval gate run
~/Documents/h-uman/build/human eval gate \
    --candidate-adapter ~/.human/adapters/persona/ \
    --baseline stock-gemma-4-26b \
    --output ~/.human/proofs/2026-05-18-persona-v8-vs-stock/

# Inspect verdict
cat ~/.human/proofs/2026-05-18-persona-v8-vs-stock/gate_decision.json
```

(The exact `--candidate-adapter` / `--baseline` flag names are TBD until
the daemon binary rebuilds and I can call `human eval gate --help`. The
above is the schema-matching command from the parent plan.)

## Cross-references

- `2026-05-18-adapter-routing-decision.md` — depends on this real
  eval to land. Adapter routing recommendation is currently
  architecture-only; should be validated empirically once Phase 5
  produces real numbers.
- `~/.claude/rules/silent-config-gated-subsystems.md` — the rule
  this session authored to prevent the kind of silent gating that
  hid the tapback-DPO pipeline. Applies symmetrically to "is the
  eval gate enabled in config" (it is, by default — `enabled` is
  not a config field for the eval gate, it's run on demand).

## Bottom line

Phase 5 is **shipped code**, not **design work**. Treat the parent
plan as accurate-architecturally and stale-on-status. The single
remaining bottleneck is **trigger a real run**. Nothing about that
needs a new spec or new architecture; it needs one CLI command and
one rebuilt binary.
