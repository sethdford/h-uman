# Critic findings — retire reflection patterns on negative reaction (commit 4fb2b621)

Status: CLOSED — findings verified against the COMMITTED blob and disposed.
3 false-positive/by-design, 2 genuine low-severity (both fixed in df663986).

## CORRECTION (2026-05-28, supersedes the first disposition)

The first disposition of this file (commit d64d7660) wrongly tagged
findings #4 and #5 as FALSE POSITIVE. Root cause: that review read the
WORKING-TREE files, but a parallel agent had already made — and not yet
committed — the exact hardening edits for #4 and #5. Reviewing the dirty
working tree instead of `git show 4fb2b621:<path>` showed someone else's
uncommitted fixes and made the genuine findings look pre-handled.

Re-checked against the committed blob `4fb2b621`:
- #4 DELETE-unchecked: VALID. In 4fb2b621 the consume DELETE was a bare
  `sqlite3_step(st);` with no error check. Low-severity (orphans are
  harmless under the retired=0 guard), but real. Fixed in df663986.
- #5 missing blast-radius test: VALID. `test_same_channel_thumbs_down_
  retires_all_in_window` did NOT exist in 4fb2b621 (`grep -c` = 0). Added
  in df663986 as AC-6, pinning the channel-scoped blast radius as intended.

## Verification disposition (corrected)

| # | Sev | Verdict | Evidence |
|---|-----|---------|----------|
| 1 | HIGH | DESIGN, not bug | Crash-safety is handled: the UPDATE carries `WHERE retired = 0` (consumer.c:389), so an orphaned surfacing after a crash cannot re-retire an already-retired pattern. The over-broad `DELETE WHERE channel=?` is the intended "consume the channel ledger on thumbs-down" semantics; blast radius documented + pinned (df663986 AC-6). |
| 2 | HIGH | FALSE POSITIVE | `int retired = sqlite3_changes(db);` is at consumer.c:401, which runs BEFORE the DELETE is prepared at :405. `sqlite3_finalize()` destroys a prepared statement; it does NOT reset the connection change counter. `sqlite3_changes()` reports the most recently *completed* DML — the UPDATE — so `retired` is the correct UPDATE row count. |
| 3 | MEDIUM | FALSE POSITIVE | The null guard already exists in 4fb2b621: `if (e->polarity == HU_REACTION_NEGATIVE && s_reflection_db)` at reaction_handler.c:297. A cleared/unset db is a no-op by construction. |
| 4 | MEDIUM→LOW | VALID, fixed | 4fb2b621 had a bare `sqlite3_step(st)` for the consume DELETE. df663986 added the `!= SQLITE_DONE` check + `hu_log_error`. Severity is LOW not MEDIUM: orphans are neutralized by the retired=0 guard, so the only impact is operator visibility of a persistent I/O fault. |
| 5 | LOW | VALID, fixed | The blast-radius test did not exist in 4fb2b621. df663986 added `test_same_channel_thumbs_down_retires_all_in_window` (pA + pB surfaced in-window → one thumbs-down retires both, lineage consumed, second thumbs-down returns 0). |

RESULT_critic_disposition=2_HIGH_FALSE_POSITIVE_1_MEDIUM_FALSE_POSITIVE_2_LOW_VALID_FIXED

Reflexion notes:
1. The critic's two HIGH findings were both false positives (it alleged
   without reading the surrounding guard + the sqlite3_changes ordering) —
   a `tune-agent` candidate per `audit-verify-before-allege`. But it was
   RIGHT about the two low-severity gaps.
2. My own first disposition erred by reviewing the working tree while a
   parallel agent held uncommitted edits. Lesson: to review commit X,
   read `git show X:path`, never the live working-tree file, whenever
   concurrent agents may have dirty state.

## Original findings (preserved for audit trail)

| Severity | file:line | concern | suggested fix |
|----------|-----------|---------|---------------|
| HIGH | src/reflection/consumer.c:381-412 | DELETE-after-UPDATE not transactional; if crash between lines 400-410, surfacing rows persist and re-retire patterns on next thumbs-down in same channel. Also: DELETE WHERE channel=? removes ALL surfacings for that channel, not just the retired ones — patterns surfaced-but-not-yet-retired or from concurrent turns lose lineage. | Wrap both queries in explicit `BEGIN TRANSACTION...COMMIT` OR collapse to single UPDATE-with-subquery-delete. Test the crash path. Clarify intent: are concurrent-turn surfacings re-retiring intended or a bug? |
| HIGH | src/reflection/consumer.c:401 | sqlite3_changes(db) read AFTER finalize of the UPDATE statement (line 400). If DELETE statement's prepare/step occurs before changes() is queried, changes() may return DELETE count instead. | Capture `int retired = sqlite3_changes(db);` immediately after UPDATE's finalize, before preparing DELETE. |
| MEDIUM | src/agent/reaction_handler.c:297-300 | s_reflection_db is a static global with implicit daemon lifecycle assumption. Reaction handler fires on every inbound reaction; if lifecycle desync occurs (e.g. test harness clears db mid-turn), silent no-op with no explicit guard. | Add defensive `if (!s_reflection_db) return HU_OK;` at line 297 to make the contract explicit. |
| MEDIUM | src/reflection/consumer.c:405-410 | DELETE statement is never error-checked. If DELETE fails (SQLITE_IOERR, etc.), surfacing rows leak silently and caller doesn't know retire was partial (UPDATE succeeded, DELETE failed). | Wrap DELETE in error check and either return error code or emit log_error + return retired count to acknowledge partial failure. |
| LOW | tests/test_reflection_retire_on_contradiction.c | Tests do NOT pin concurrent-turn blast-radius or DELETE-after-UPDATE ordering. Missing test case: two patterns surfaced from different turns in same channel → one thumbs-down retires both. | Add test: insert pattern_A (T1), insert pattern_B (T2), surface both, thumbs_down → verify both retired AND no re-retire on next thumbs_down. Pins whether concurrent retirement is intended. |

## Cross-agent regression risk

(none observed)

# Critic findings — US-101 Bradley-Terry reward model (HUML)

Status: COMPLETE — reviewed diff at commit 5dca845e..HEAD

## Findings

| Severity | file:line | concern | suggested fix |
|----------|-----------|---------|---------------|
| HIGH | tests/test_reward_model_huml.c:193 | Hardcoded stack buffer `float analytic[100]` assumes hidden_dim==100. If a future test uses hidden_dim≠100, buffer overflow. | Replace `float analytic[100]` with dynamic allocation: `float *analytic = alloc->alloc(alloc->ctx, V * sizeof(float));` and corresponding free in cleanup. Or assert at line 200 that V==100 before using the static buffer. |
| MEDIUM | src/ml/reward_model_train.c:162-167 | OOM error path calls free() on potentially-NULL pointers (partial allocation failure). If h_w succeeds but h_l fails, then alloc->free(ctx, h_l, ...) is called on NULL, which may not be safe in a custom tracking allocator. | Guard: only call free() on pointers that actually allocated, or pre-zero all five pointers at line 156 and free ALL at end (already done at 192-196, so keep both cleanup paths). Safe in system allocator (free(NULL) is no-op), but defensive against custom allocators. |
| MEDIUM | tests/test_reward_model_huml.c:215 | Gradient-check tolerance 2% (rel < 2e-2) is loose; could a sign error pass? Yes IF weights near zero, but the skip condition (line 212) bails out on both <1e-3. With typical sigmoid~0.7, sign error produces 100% rel error → test catches it. BUT the skip condition silently exempts ill-conditioned weights. Document why this tolerance was chosen (float32 rounding limits?), or tighten after confirming gradients are actually correct. | Add comment at line 215 explaining the 2% choice: "float32 rounding in value_head_backward adds ~0.5-1% error; 2% allows margin for accumulation. Ill-conditioned (a,fd both <1e-3) skipped because relative error undefined." Or: verify by hand one gradient computation on a non-trivial weight and document. |
| LOW | src/ml/reward_model.c:199 | When out_h != NULL in huml_score_core, a memcpy copies the hidden vector. But the comment at line 112 says "no extra copy needed", contradicting the design intent that value_head_forward should receive the raw last-position logits without buffering. The memcpy is safe (it's to caller's buffer), but it's wasteful if the caller only needs the score. Optimize: pass out_h directly to value_head_forward when available? | Minor optimization: check if hu_value_head_forward can accept the hidden vector in-place (without copy). If value_head_forward doesn't mutate the input, pass the pointer directly. If copy is required for internal reasons, keep as-is and document. |
| LOW | tests/test_reward_model_huml.c | AC-101.7 (batch scoring <100ms latency target) is not explicitly tested. The design specifies "manual run", but no assertion or benchmark exists. Future changes might regress latency silently. | Add a comment documenting the latency target (100ms/100 pairs on dev machine), or add a test that times batch scoring and logs the result (non-asserting, for visibility). |

## Cross-agent regression risk

None observed. The change is isolated to reward_model* modules and test suite. No changes to provider, channel, tool, or security boundaries.

# Critic findings — US-101 Bradley-Terry reward model (HUML)

Status: COMPLETE — reviewed diff at commit 5dca845e..HEAD

## Findings

| Severity | file:line | concern | suggested fix |
|----------|-----------|---------|---------------|
| HIGH | tests/test_reward_model_huml.c:193 | Hardcoded stack buffer `float analytic[100]` assumes hidden_dim==100 forever. Current code only calls gradient_check_against_fd via run_gradient_check_for_batch (line 235-240, which hardcodes hidden_dim=100), so NO BUFFER OVERFLOW TODAY. BUT latent bug: if future PR refactors to call gradient_check_against_fd directly with hidden_dim≠100, stack buffer overflows. Loop at line 200 unconditionally goes to j < 100, writing beyond buffer if V < 100 (safe) or if called with hidden_dim > 100 buffer is too small. | Replace stack buffer with dynamic allocation: line 193 change to `float *analytic = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));` where V = ctx->value_head.hidden_dim. Add corresponding free() call in gradient_check_against_fd error/cleanup paths (before all returns at lines 218, or after loop at line 217). |
| MEDIUM | src/ml/reward_model_train.c:162-167 | OOM error path calls free() on potentially-NULL pointers from partial allocation failures. If h_w succeeds but h_l fails (line 158), cleanup at line 164 calls alloc->free(ctx, h_l, ...) where h_l==NULL. While system allocator free(NULL) is safe (no-op), project's established pattern (src/ml/lora.c lines ~140) guards each free with `if (ptr)` before freeing. Inconsistency violates defensive coding discipline and may fail with custom tracking allocators. | Pre-zero all pointers at lines 157-161: change to `float *h_w = NULL, *h_l = NULL, *dW_w = NULL, *dW_l = NULL;` and `double *total_dW = NULL;` (before line 157). Then guard cleanup at 163-167: `if (h_w) alloc->free(ctx, h_w, ...); if (h_l) alloc->free(...);` etc. Matches lora.c pattern and defensive allocator contract. |
| LOW | src/ml/reward_model.c:112-199 | Comment at line 112 claims "no extra copy needed for the hidden state", but line 199 does memcpy(out_h, h, V * sizeof(float)). The copy is safe (to caller's buffer) but contradicts comment. Clarity issue. | Clarify comment at line 112: document whether the copy is necessary (lifetime, mutability) or wasteful. If copy not needed, pass h directly to hu_value_head_forward (verify forward() doesn't mutate input). If copy is needed, update comment to explain why. |
| LOW | tests/test_reward_model_huml.c | AC-101.7 latency target (100ms/100 pairs on dev machine per design:153) documented in spec but not tested. No assertion, benchmark, or comment in test code. Future changes might regress latency silently. | Add inline comment at top of test file or in test_score_batch_two_sided documenting the latency contract: "AC-101.7: batch scoring of 100 pairs should complete in <100ms on dev machine (manual verification only; no automated assertion)". Helps reviewers understand performance expectations. |

## Cross-agent regression risk

None observed. Changes isolated to reward_model{,_train,_priv}.[hc] and test_reward_model_huml.c. No changes to provider, channel, tool, security, or eval boundaries.

RESULT_critic=HAS_FINDINGS severity=HIGH count=1
