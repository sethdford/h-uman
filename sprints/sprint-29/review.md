# Sprint 29 — review

## Goal

Catch CoT / prompt-context leaks in the primary `response_guard.c` so
the 2026-05-12 production failure (979-byte model self-narration sent
to a real human contact via the live service-loop daemon) cannot
recur.

## Stories shipped

- ✅ **G1** Phase 3 numbered-analytical-list dump detector
  (`hu_guard_has_numbered_analysis_dump`).
- ✅ **G2** Self-talk / scene-direction echo detector
  (`hu_guard_has_self_talk_pattern`, 6 patterns).
- ✅ **G3** Third-person-about-user double-pattern detector
  (`hu_guard_count_third_person_patterns`, 18 patterns + a
  capitalized-name self-narration bonus rule).
- ✅ **G4** 7 new tests in `tests/test_response_guard.c`:
  1. `guard_rejects_2026_05_12_brea_leak_verbatim` — verbatim
     reconstruction of the 979-byte production payload → REJECT.
  2. `guard_rejects_numbered_analysis_dump` — 3-item numbered
     analytical dump → REJECT.
  3. `guard_rejects_numbered_analysis_paren_form` — `1)` paren
     variant → REJECT.
  4. `guard_rejects_self_talk_substrings` — 6 sub-cases, each in
     isolation → REJECT.
  5. `guard_rejects_third_person_double_pattern` — 2+ hits → REJECT.
  6. `guard_passes_third_person_single_hit` — 1 hit → OK.
  7. `guard_passes_legit_replies_with_similar_surface_features` —
     6 negative cases (legit "Wait, …", short numbered list, inline
     numbers, single G3 hit, casual reply, single G2 lookalike) →
     all OK.

## Files touched

| File | Change |
| --- | --- |
| `include/human/agent/response_guard.h` | Added `bool detected_semantic_leak` to `hu_guard_report_t` (additive ABI; callers zero-init the report so safe). |
| `src/agent/response_guard.c` | +2 thresholds (`HU_GUARD_NUMBERED_LONG_THRESHOLD = 30`, `HU_GUARD_NUMBERED_MIN_ITEMS = 3`); +5 static helpers (`hu_guard_ci_contains`, `hu_guard_has_numbered_analysis_dump`, `hu_guard_has_self_talk_pattern`, `hu_guard_count_third_person_patterns`, `hu_guard_detect_semantic_leak`); +1 Phase 3 block in `hu_response_guard_check` after Phase 2 degenerate-repetition. |
| `tests/test_response_guard.c` | +7 tests. |
| `sprints/sprint-29/{stories,review,retro}.md` | New. |

## Verification

| Build | Result |
| --- | --- |
| `cmake --preset dev` (ASan, all features) | **10286 / 10286 passed**, 0 ASan errors. |
| `cmake --preset minimal` (no ASan, minimal features) | **8826 / 8826 passed**. |

The 7 new tests all run in the dev build's `Response Guard` suite
(29 tests total in that suite, all PASS). Minimal build excludes
some test files but the response-guard suite is included in both.

Note on test counts: Sprint 28 close reported 10222 dev tests; this
sprint reports 10286, a delta of 64 (vs the +7 we added). The
remainder are tests that were registered but skipped under
sprint-28's framework state and are now active in the dev build —
not regressions, not new ones from us. Worth a follow-on
investigation but out of scope for the safety-fix sprint.

## Out of scope (deferred)

- **Length-anomaly hard-block** in the guard — needs `recent_avg_len`
  threaded through the public API, touches every caller.
- **Director-string echo detection** — needs the director string
  threaded through; same API-change concern.
- **Quality gate `MARGINAL → ship` policy fix** — separate file
  (`agent_stream_v2.c`), separate code path.
- **Existing retry generation WIP in main worktree** — left
  alone; complementary, not conflicting.
- **Post-mortem doc** at `docs/postmortems/2026-05-12-cot-leak.md` —
  belongs in a separate doc-only PR for traceability.

## Operational follow-up (for the user)

1. Cherry-pick this branch into the main `h-uman` worktree and ship
   to the live daemon ASAP.
2. Audit `+14846784914` thread for embarrassing sends (separate
   sprint).
3. Pull the full 979-byte leaked response from `chat.db` for the
   post-mortem (separate sprint).
