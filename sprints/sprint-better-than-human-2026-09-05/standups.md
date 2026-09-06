# Standups — sprint-better-than-human-2026-09-05

Working directory: `.claude/worktrees/sprint-better-than-human-2026-09-05`, branch
`sprint-better-than-human-2026-09-05`, base `d5c0257b8` (origin/main). Per-story
worktrees `sbth-usN` on branches `sbth-usN`. Gate order per story:
implementer commit → verifier → critic (≤2 rounds) → aspect panel → merge (one at a
time, tests between).

## 2026-09-05 (after W3/W4 dispatch, 4 of 8 merged)

| Story | Wave | Status | Tip | Blocker |
|---|---|---|---|---|
| US-1 | W1 | DONE — merged `ca0b04450` | `4f064a3df` | none |
| US-3 | W2 | DONE — merged `bb319aae9` | `1add61bef` | none |
| US-6 | W2 | DONE — merged `8ccd50b0f` | `24603b998` + evidence `4682701f3` | none |
| US-7 | W2 | DONE — merged `590a3fc11` | `7adbb0d23` + evidence `6a0ada146` | panel regression aspect timed out (weight 0); critic-2 covered regression |
| US-2 | W2 | critic → fix in progress (F1 noise-floor: three-way HOLD verdict) | `74b95e440` | AC-2.6 needs the next retrain window (PARTIAL by design) |
| US-4 | W3 | critic MERGE-WITH-FIXES → fix in progress (F1 send_failed MIR symmetry, F2 diagnostics on refusal) | `fb816f7f3` | verifier PASS; measurement honestly REFUSED (FIR n=2 < 30) |
| US-5 | W3 | fix landed → verifier r2 + critic r2 running | `58efb051a` | round-1 BLOCK (empty stubs, no harness mode); live paired measurement is a coordinated step |
| US-8 | W4 | implementer DONE → verifier + critic running | `ddf03cb99` | server-side generation in eval script deferred (dry-run + gate logic only) |

Callouts:
1. No story has exceeded 2 critic rounds. US-5 and US-2 are on round 2.
2. Ratchets green on every story that reported (clone 11447, daemon.c 12313, agent-core 4).
3. Deviation from plan §2: waves overlapped in time (W2 tails ran alongside W3/W4)
   because every story has its own worktree and no two touch the same file; the
   C-code stories (US-5 `hybrid.c`, US-8 `model_router.c`) will still merge one at a
   time with the full suite between.
4. Verifier round 1 on US-5 reached `:8741` twice because the dispatch prompt did not
   pin `HU_SEMANTIC_EMBED_URL`; every later dispatch pins it. Recorded in lessons.

## 2026-09-05 (later) — gate updates

| Story | Update |
|---|---|
| US-2 | fix `7acb4d470` landed (three-way PASS/BLOCK/HOLD using the candidate's LUAR CI95; HOLD exit 3; INCONCLUSIVE on missing CI). Combined verifier+critic round 2 running. |
| US-4 | fix `8beccb6ed` landed (F1 symmetric MIR send semantics, F2 diagnostic counters on refusal; 28 tests). Aspect panel running. |
| US-8 | critic round 1 **BLOCK**: `/admin/models/decisions` per-row array still emits shadow entries while `tier_distribution` filters them (cross-consumer regression); no test compares gate-OFF vs gate-SHADOW selections for AC-8.7; gate comment does not name the measurement. Fix dispatched (round 2 of 2). |

## 2026-09-05 (evening) — 7 of 8 merged

| Story | Merge commit | Gates |
|---|---|---|
| US-4 | `c307ee26e` | verifier PASS, critic fixed (`8beccb6ed`), panel PASS 1.0 |
| US-5 | `046ba18ef` | verifier r2 PASS (14320/14320), critic r2 MERGE, panel PASS 1.0 (regression aspect timed out, weight 0) |
| US-2 | `00cd842c8` | verifier r2 PASS, critic r2 MERGE, panel PASS 1.0 |

Merged-tree checks: 467 pytest + 39 `test_m3_promote.py` green after US-2; full C suite
running on the sprint tree after US-5 (first C story to land).

US-8: verifier round 1 PASS but it ran on a tree the fix agent was concurrently editing
(it cites the AC-8.7 test the fix adds), so it is informational only; a clean re-verify
runs after the fix commit lands. Critic round 2 follows the fix.

## 2026-09-05 (close of execution) — 8 of 8 merged

| Story | Merge commit | Gates |
|---|---|---|
| US-8 | `1f619c866` | verifier r2 PASS (14314/14314 on story tree; AC-8.7 mutation-proved), critic r2 MERGE-WITH-FIXES (ratchet history) → resolved by squash `7a6cb1e30` (tree byte-identical), panel PASS 5/5 across two runs (first run ERRORED on the $2.50 panelist budget; raised to $6.00 via `ASPECT_PANEL_MAX_BUDGET`) |

Completed sprint tree `1f619c866`: full C suite **14329/14329 passed, 6 skipped, 0 ASan
errors**; ratchets clone 11447/11447, daemon.c 12313/12313, agent-core factory 4 / memcmp 0,
sqlite includers 97/97, loose root files 4/4; pytest **478 passed**.

Process incidents worth the retro: US-5 implementer reported DONE with four empty test
stubs (caught by critic+verifier); US-5 verifier r1 reached `:8741` because the prompt did
not pin the embed URL; US-8 implementer raised `CLONE_BASELINE` mid-fix then restored it with
invented provenance (caught by lead diff, squashed); typed agents hit turn caps (used
general-purpose with the role in the prompt); aspect-panel budget too low for a C story.
