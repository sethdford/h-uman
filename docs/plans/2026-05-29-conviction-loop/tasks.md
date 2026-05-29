# Close the Conviction Loop — Tasks

> Follows `requirements.md` (ACs) + `design.md`. Each task lists the AC it
> satisfies and how it's verified. TDD: write the pinning test first where
> marked ⓣ. Order respects dependencies.

## T1 — Decision predicate (pure) ⓣ  → AC-3, AC-2, AC-4
- [ ] Create `include/human/agent/belief_update.h`: `hu_belief_update_t`
      enum, `hu_belief_facts_t` struct, `hu_belief_update_decide` proto.
- [ ] Create `tests/test_belief_update.c` FIRST. Pin the full truth table:
      - stance_exists=F → NO_CHANGE
      - has_new_evidence=F → NO_CHANGE
      - is_reassertion=T (+evidence) → NO_CHANGE  (AC-2)
      - evidence non-contradict, conviction any → STRENGTHEN
      - contradict + conviction≤0.7 → FLIP; contradict + >0.7 → WEAKEN
      - changes_this_convo≥2 → NO_CHANGE  (AC-4)
      ≥ 8 cases (N inputs + 2).
- [ ] Implement `hu_belief_update_decide` in `src/agent/belief_update.c`
      until the table is green. No SQLite, no agent state in signature.
- **Verify:** `./build/human_tests --filter=belief_update` all pass.

## T2 — Evidence-cue + conviction helpers (pure) ⓣ  → AC-1, AC-3
- [ ] Add `hu_belief_msg_has_evidence_cue(msg,len)` (because/data/study/
      actually/source/turns-out markers) + `hu_belief_conviction_for(d,cur)`
      (STRENGTHEN +0.2 cap1.0; WEAKEN −0.2 floor0.0; FLIP 0.55) to the TU.
- [ ] Tests: evidence cue positive/negative; conviction map per enum value.
- **Verify:** filter run green; reassertion-veto case (cue present BUT
      is_reassertion=T) resolves to NO_CHANGE via T1.

## T3 — CMake / gate symmetry  → AC-8
- [ ] Register `src/agent/belief_update.c` + `tests/test_belief_update.c`
      in `CMakeLists.txt`; forward-decl + call `run_belief_update_tests()`
      in `tests/test_main.c`. SQLite gate matches `evolved_opinions`
      (`.claude/rules/test-source-gate-symmetry.md`).
- **Verify:** `bash scripts/check-test-source-gate-symmetry.sh` exit 0;
      `bash scripts/check-test-references.sh` exit 0.

## T4 — Wire belief-update post-response ⓣ  → AC-1, AC-4
- [ ] In `src/daemon.c` after `hu_evolved_opinions_extract_and_store`
      (~:12120): build `hu_belief_facts_t` from
      `hu_pressure_history_inspect` + evidence cue + `hu_evolved_opinion_find`
      (for stance_exists/current_conviction) + per-convo change counter;
      call `hu_belief_update_decide`; on non-NO_CHANGE call
      `hu_evolved_opinion_upsert_with_history`.
- [ ] Integration test (SQLite, deterministic): held topic + new-evidence
      msg → exactly **one** `opinion_history` row with non-empty
      `change_reason` (AC-1). Increment per-convo counter; assert 3rd
      change suppressed (AC-4).
- **Verify:** `touch src/daemon.c && cmake --build build --target human -j8`
      shows "Linking C executable human"; filter test green.

## T5 — Reassertion resists (anti-sycophancy regression) ⓣ  → AC-2
- [ ] Test: same-content user push ×3 on a held topic → **zero**
      `opinion_history` rows AND existing antisycophancy firmness does not
      decrease (assert via trust calibrate path / pressure_history).
- [ ] Confirm NO edit to `behavior/trust.h`.
- **Verify:** new test + full existing antisycophancy suite green
      (`.claude/rules/tests-that-pin-bugs.md`).

## T6 — Shift-narrative injection  → AC-5
- [ ] Stash the directive returned by `upsert_with_history` on the
      session; inject into next turn's system prompt using the
      `agent_turn.c:2698` realloc pattern.
- [ ] Test: after a belief update, the shift directive string is present
      in the assembled prompt on the following turn. Free on consume.
- **Verify:** filter test green; 0 ASan errors.

## T7 — Firmness regression guard ⓣ  → AC-6
- [ ] Test pins `hu_evolved_opinion_build_directive` mapping: conviction
      0.9→"firmly", 0.6→"moderately", 0.3→"tentatively". No source change.
- **Verify:** filter test green (guards existing behavior).

## T8 — belief_flexibility eval metric ⓣ  → AC-7
- [ ] Add `belief_flexibility` score to `src/eval.c` + `include/human/eval.h`
      beside antisycophancy. + on genuine-evidence update; − on never-update
      across evidence turns OR update-on-reassertion.
- [ ] Rubric unit tests for both extremes (wall, pushover) + the good case.
- **Verify:** `--suite=eval`-style filter green.

## T9 — Full-suite gate + memory  → AC-8
- [ ] `cmake --build build -j8 && ./build/human_tests` → 0 failures, 0 ASan.
- [ ] `/verify` (verifier agent) on the belief-update behavior: held-topic +
      evidence flips/strengthens; reassertion does not. Capture RESULT=PASS.
- [ ] Update auto-memory: conviction loop closed, belief_flexibility live.

## Done definition
All 8 ACs checked; full suite green + ASan clean; verifier PASS captured;
no edit to `behavior/trust.h`; gate-symmetry + test-reference scripts pass.

## Suggested dispatch
Single `/team` or sequential single-agent (T1→T2→T3 pure/cheap; T4–T6 touch
`daemon.c`/`agent_turn.c` — same files, so **sequential**, not parallel, per
`verify-worktree-isolation-before-fanout.md`). T7/T8 are independent and may
run in parallel with each other.
