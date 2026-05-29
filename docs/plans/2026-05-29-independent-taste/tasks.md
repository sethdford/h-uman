# Independent Taste (A2) — Tasks

> Follows `requirements.md` + `design.md`. ⚠ T0 (ethics review) gates all code.
> Mirrors the verified A1 build order: pure predicates → store → wire → eval.

## T0 — Ethics/honesty review gate (BLOCKS all code) → AC-6, constraints
- [ ] Produce a 1-page threat note: can persistent independent taste manipulate
      or deceive the user? Confirm taste is suppressible, never overrides
      instructions/accuracy/safety, never claims sentience.
- [ ] Append the note to `design.md`. Sign-off required before T1+.

## T1 — Expression predicate (pure) ⓣ → AC-3
- [ ] `include/human/persona/taste.h`: valence enum, `hu_taste_pref_t`,
      `hu_taste_express_facts_t`, `hu_taste_express_decide`.
- [ ] `tests/test_taste.c` first: truth table (relevance × strength ×
      anti-repetition), ≥ N+2 cases.
- [ ] Implement predicate in `src/persona/taste.c`. Pure, no SQLite.

## T2 — Stability + drift predicates (pure) ⓣ → AC-4, AC-5
- [ ] `hu_taste_should_revise` returns false for mere disagreement (AC-4).
- [ ] `hu_taste_drift_step` rate-limited/coherent, voice_maturity-paced (AC-5).
- [ ] Tests: disagreement does not collapse strength; multi-step drift bounded.

## T3 — Store (SQLite-gated) ⓣ → AC-1
- [ ] `taste_prefs` table + `ensure/upsert/get`. Round-trip tests.
- [ ] Gate symmetry + test-reference scripts pass.

## T4 — Independent starter seed ⓣ → AC-1, AC-2
- [ ] Author starter taste JSON (8–12 prefs, NOT derived from Seth).
- [ ] Load on first run into `taste_prefs`.
- [ ] Test: starter loads; entries present with origin marker.

## T5 — Isolation guard (release-blocking) ⓣ → AC-2
- [ ] Test: run the Seth-style-learning path (`style_learner`/`style_mirror`);
      assert ZERO rows written to `taste_prefs`. Confirm those modules get no
      handle to the taste store.

## T6 — Expression rendering + honesty contract ⓣ → AC-3, AC-6
- [ ] `hu_taste_build_directive` (leaked-taste framing, not pronouncement).
- [ ] Honesty test: directive contains no "I feel/I'm conscious"-class strings;
      compatible with `docs/standards/ai/` disclosure.

## T7 — Turn wire → AC-3
- [ ] Inject taste directive in `agent_turn.c` (realloc-append pattern). Free
      on consume. ASan clean.

## T8 — distinctiveness eval metric ⓣ → AC-7
- [ ] `hu_eval_score_distinctiveness` beside antisycophancy/belief_flexibility.
- [ ] Rubric tests: mirrors-Seth (low) vs stable-own-taste (high).

## T9 — Full gate → AC-8
- [ ] Full suite green + 0 ASan; `/verify`; gate-symmetry + test-ref pass.
- [ ] Verify in an ISOLATED worktree (concurrent process active on this branch —
      see `verify-worktree-isolation-before-fanout.md`).

## Dispatch
T1–T2 pure/parallelizable. T3–T7 share `persona/taste.c` + `agent_turn.c` →
SEQUENTIAL. T8 independent. Recommend `/spec`-then-`/team` AFTER T0 sign-off.
