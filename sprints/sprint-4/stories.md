---
title: "Sprint 4 — M2 measurement bundle (multi-turn simulation)"
created: 2026-05-12
status: closed
sprint: 4
branch: sprint-4-m2-measurement
working_directory: /Users/sethford/Documents/human-sprint-4
---

# Sprint 4 — M2 measurement bundle

## Sprint goal

Promote M2 (Personal Model) from "shipped — trust us, it works" to "shipped + measured" by adding the **multi-turn behavioral simulation** that the existing 60+ unit tests don't cover. Existing tests probe single functions (one ingest, one fact, one decay). What's missing is the integration story: **does the model, driven through a realistic 50-turn conversation, end up in the state CLAUDE.md claims?**

Out of scope: extending fact extraction, tweaking decay constants, adding new `hu_personal_model_*` APIs, persona example bank growth, or any provider work.

## Sprint number

This sprint is **sprint-4**. `sprint-3` was claimed by a concurrent agent (Track E Phase 2 security hardening) before this sprint started — see `sprints/sprint-3/stories.md`. To avoid collision, this sprint uses sprint-4 directly.

## Pre-sprint state (read at Phase 0)

`sprint-status.sh` showed:
- 24 dirty files (8 modified + 16 untracked) across `src/agent/`, `src/gateway/`, `src/memory/lifecycle/`, `src/tools/`, `tests/` — concurrent activity is HIGH.
- 4 active sprint-shaped branches (sprint-1-fidelity-followthrough, sprint-2a-hygiene-baseline, sprint-2b-personal-model-honesty, sprint-2c-followups).
- F4's worktree threshold triggered — this sprint runs in `../human-sprint-4` worktree.

Existing test inventory in `tests/test_personal_model.c` (3308 lines, ~70 `static void` test fns):
- ✅ Single-fact ingest extracts facts
- ✅ Merge / dedup (3 tests)
- ✅ `fact_effective_confidence` — no_decay/halves/quarters/floors/null (5 tests)
- ✅ `topic_effective_score` — halves/floors/null (5 tests)
- ✅ `goal_effective_priority` — halves/null/inactive/empty/fallback (5 tests)
- ✅ `style_freshness` — halves/null/never_observed/unstamped (4 tests)
- ✅ Save/load round-trip (one file at a time)
- ✅ Crash safety + sigkill survival
- ✅ Style directives (~6 variants)
- ✅ Avoid lines, topic directives, chronotype inference

What's NOT covered:
- Multi-turn conversation flow (drive ingest 50 times, check intermediate state).
- Drift over simulated days (jump clock forward, observe decay impact on prompt output).
- Stress / invariant testing (1000 random turns, assert array bounds + score ranges).
- Save/load AFTER long simulation (existing test saves a freshly-init'd model).

## Stories

### Story B1 — 50-turn deterministic simulation harness

**File:** new `tests/test_personal_model_simulation.c` (registered in `tests/test_main.c` and `CMakeLists.txt`).

Drives `hu_personal_model_ingest` through a fixed 50-turn fixture spanning 14 simulated days and asserts checkpoint state at turns 1, 10, 25, 50.

- **AC-B1.1**: Fixture is a `static const struct { const char *text; int from_user; int day_offset; int hour; }` array of 50 entries — deterministic, in-source, no JSON parsing or fixture file.
- **AC-B1.2**: Fixture covers realistic chat patterns: identity ("i work at initech", "my name is alex"), preferences ("i love hiking", "i hate mornings"), goals ("i want to ship the deck this quarter"), repeated topics, style varying by turn (lowercase, abbrev, longer/shorter).
- **AC-B1.3**: Test calls `hu_personal_model_ingest` for each turn with a synthetic timestamp `T0 + day_offset*86400 + hour*3600`.
- **AC-B1.4**: After turn 50, asserts:
  - `fact_count > 0` and includes specific subject/predicate/object triples we expect (identity, preference, dislike).
  - `topic_count > 0` and includes the repeated-topic items.
  - `style.sample_count >= 25` (only user turns count toward style, ~25/50 fixture turns are user).
  - `style.last_observed_at == ` final user turn timestamp.
  - `interaction_count == 50`.
- **AC-B1.5**: Build prompt at turn 50 returns non-empty, contains the expected fact summary line, and stays under buf cap.

### Story B2 — Drift / time-travel regression

**File:** add to `tests/test_personal_model_simulation.c`.

After running B1's 50-turn simulation, advance simulated time by 180 days (one fact half-life × 2). Assert that effective scores halve and the rebuilt prompt drops decayed signal.

- **AC-B2.1**: Capture the turn-50 prompt buffer.
- **AC-B2.2**: Capture turn-50 effective fact confidence, topic interest, goal priority, style freshness for a specific reference fact / topic / goal / style.
- **AC-B2.3**: Advance `now` by `HU_FACT_CONFIDENCE_HALF_LIFE_SEC` (90 days). Re-evaluate effective scores — fact's effective confidence is between 0.45× and 0.55× original; topic between 0.20× and 0.30× (60-day topic half-life over 90 days = ~2× one half-life × 1.5x factor → ~0.35× ; let me re-check: `0.5^(90/60) ≈ 0.354`); goal between 0.55× and 0.65× (`0.5^(90/120) ≈ 0.595`); style freshness between 0.65× and 0.75× (`0.5^(90/180) ≈ 0.707`).
- **AC-B2.4**: Build prompt at the advanced time. Assert it's smaller than the turn-50 prompt OR contains a "drops stale" signal (e.g., the avoid-line is gone, or the topic directive is gone).
- **AC-B2.5**: Advance by 360 more days (total 1.5 years from turn-50). Assert effective fact confidence ≈ 0 (floored), prompt no longer contains the original fact summary.

### Story B3 — 1000-turn invariant stress test

**File:** add to `tests/test_personal_model_simulation.c`.

Drive 1000 deterministic-pseudorandom turns through ingest. After every 100 turns, assert that bounded-state invariants hold.

- **AC-B3.1**: Use a deterministic PRNG (libc `srand(42)` is fine; ASCII-safe random text) so the test is reproducible across runs.
- **AC-B3.2**: At every checkpoint (turns 100, 200, …, 1000), assert:
  - `model->fact_count <= HU_PM_MAX_FACTS` (saturation, not overflow).
  - `model->topic_count <= HU_PM_MAX_TOPICS`.
  - `model->goal_count <= HU_PM_MAX_GOALS`.
  - For each fact, `0 <= confidence <= 1` and `effective_confidence(now)` in `[0, 1]`.
  - For each topic, `0 <= interest_score <= 1` and `effective_score(now)` in `[0, 1]`.
  - For each active goal, `0 <= effective_priority(now) <= 1`.
  - Style: `0 <= formality, verbosity, emoji_frequency, humor_receptivity, lowercase_ratio, abbreviation_ratio <= 1`.
- **AC-B3.3**: Build prompt at every checkpoint with a 4 KB cap. Asserts it returns within the cap, returns `> 0` bytes, and is NUL-terminated. (Existing tests don't pin all three together at the saturated state.)
- **AC-B3.4**: Total test runtime < 5s on a typical dev box (the loop is in-memory; no I/O).

### Story B4 — Save/load round-trip after long simulation

**File:** add to `tests/test_personal_model_simulation.c`.

Existing `personal_model_save_load_round_trips` saves a freshly-init'd model and reloads it. This test saves AFTER a 50-turn simulation, reloads, simulates another 50 turns, and asserts no state is lost across either cycle.

- **AC-B4.1**: Run B1's 50-turn simulation → call `hu_personal_model_save` to a `mktemp`-style path under `${TMPDIR}` → re-init a second model → `hu_personal_model_load` → assert second model byte-equivalent to first (at the public-field level: fact_count, topic_count, goal_count, style.sample_count, interaction_count, all timestamps).
- **AC-B4.2**: Call `hu_personal_model_build_prompt` on the loaded model with the same `cap`. Asserts the bytes are equal to the pre-save prompt.
- **AC-B4.3**: Run a second 50-turn simulation on the loaded model, save again, reload again. Final state still byte-equivalent.
- **AC-B4.4**: Cleanup: remove the temp file in test teardown via `unlink`.

## Stories explicitly NOT in this sprint

- **Eval CLI subcommand `human eval personal-model-simulation`** — `src/eval.c` is large and provider-driven; adding a non-provider subcommand requires non-trivial scaffolding. The simulation is runnable via `./build/human_tests --filter=simulation` which is sufficient for CI integration. Defer to a future sprint.
- **Cross-channel coherence** — depends on persona overlay surface; currently being worked on by another agent (`include/human/persona/steering.h` is untracked). Defer to avoid collision.
- **MLX provider integration** — concurrent agent territory.

## Definition of Done

- All four AC blocks (B1-B4) covered by tests in `tests/test_personal_model_simulation.c`.
- Test file builds, all new tests pass under ASan with zero leaks.
- New tests registered in `tests/test_main.c` and `CMakeLists.txt`.
- Implementer commits each story before handoff (Sprint 2a protocol).
- Per-story documentation in commit messages (what's tested, why it matters).
- Sprint closed with `review.md` + `retro.md`.
