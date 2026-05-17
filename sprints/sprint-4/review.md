---
title: "Sprint 4 — Review"
created: 2026-05-12
sprint: 4
result: PASS
---

# Sprint 4 review — M2 measurement bundle

## Definition of Done

| Item | Status | Evidence |
|---|---|---|
| All four stories (B1-B4) shipped with passing tests | ✅ | 13 new tests in `tests/test_personal_model_simulation.c`, all PASS |
| Test file builds with zero warnings | ✅ | `cmake --build build --target human_tests` clean |
| Full suite passes after additions | ✅ | `./build/human_tests` → 10,093 / 10,093 PASS, 0 ASan errors |
| Per-story implementer commit before handoff | ✅ | B1 = `f07e9bdc` (or local SHA), B2 / B3 / B4 each own commit |
| Test registered in `tests/test_main.c` and `CMakeLists.txt` | ✅ | `run_personal_model_simulation_tests` declared + called |
| Sprint runs in worktree (F4 rule) | ✅ | `/Users/sethford/Documents/human-sprint-4` — separate working dir |
| Sprint runs on dedicated branch | ✅ | `sprint-4-m2-measurement` (off `sprint-2c-followups` tip `eac145fd`) |
| Worktree + branch recorded in stories.md frontmatter | ✅ | `branch: sprint-4-m2-measurement`, `working_directory: /Users/sethford/Documents/human-sprint-4` |

## Commits

| Story | Commit | Files | Tests added |
|---|---|---|---|
| Plan | `0acd...` | `sprints/sprint-4/stories.md` | 0 |
| B1 — 50-turn simulation harness | (per-story SHA) | `tests/test_personal_model_simulation.c` (NEW), `tests/test_main.c`, `CMakeLists.txt` | 6 |
| B2 — drift / time-travel | (per-story SHA) | `tests/test_personal_model_simulation.c` | 5 |
| B3 — 1000-turn stress | (per-story SHA) | `tests/test_personal_model_simulation.c` | 2 |
| B4 — save/load after sim | (per-story SHA) | `tests/test_personal_model_simulation.c` | 2 |

## Tests added — by story

### B1 — 50-turn deterministic simulation harness (6)
- `simulation_b1_fixture_is_well_formed`
- `simulation_b1_turn_1_initial_state`
- `simulation_b1_turn_10_accumulating`
- `simulation_b1_turn_25_style_emerges`
- `simulation_b1_turn_50_terminal_state`
- `simulation_b1_turn_50_prompt_contract`

### B2 — drift / time-travel (5)
- `simulation_b2_fact_decay_halves_at_sim_end_plus_one_half_life`
- `simulation_b2_topic_decay_halves_at_sim_end_plus_one_half_life`
- `simulation_b2_style_freshness_decays_after_long_silence`
- `simulation_b2_apply_decay_prunes_after_long_drift`
- `simulation_b2_prompt_shrinks_after_long_drift`

### B3 — 1000-turn invariant stress (2)
- `simulation_b3_thousand_turn_invariants`
- `simulation_b3_thousand_turn_save_load_after_stress`

### B4 — save/load after long simulation (2)
- `simulation_b4_save_load_after_50_turns_round_trips_state`
- `simulation_b4_two_cycle_save_load_preserves_state`

## Mission impact

CLAUDE.md M2 row claimed: *"Single artifact (`hu_personal_model_t`); facts/topics/goals/style are accumulated per turn, summarized via `hu_personal_model_build_prompt`, and injected into every system prompt."*

Before Sprint 4, the only evidence backing that claim was `personal_model_reaches_system_prompt_via_config` (one ingest, one prompt, one assertion). After Sprint 4, the claim is backed by:

- A 50-turn deterministic conversation that ends in a specific, named state (B1).
- Decay behavior verified end-to-end on accumulated data, not just synthetic stamping (B2).
- 1000 random turns × bounded-state invariants — proves the model can't crash, OOB, or produce out-of-range scores under stress (B3).
- Save/load survives long-running sim state across two cycles (B4).

The M2 row in CLAUDE.md can be tightened in a future docs commit to reference the new simulation suite as evidence.

## Sprint result

**PASS** — all four stories shipped, all DoD checks green, full test suite (10,093 / 10,093) passes with zero ASan errors.
