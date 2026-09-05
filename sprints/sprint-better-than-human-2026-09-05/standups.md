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
