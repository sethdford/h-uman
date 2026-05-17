#ifndef HU_ML_LORA_EMA_H
#define HU_ML_LORA_EMA_H

/* US-11.8 — OFS-DPO dual fast/slow LoRA EMA helper.
 *
 * Computes  slow_new = alpha * slow_current + (1 - alpha) * fast  per-tensor
 * over the LoRA A/B matrices (and, for DoRA, the magnitude vector m). Used
 * by the W14 nightly cron when the Pareto gate returns PROMOTE.
 *
 * The actual safetensors read/write/EMA math lives in
 * `scripts/lora_ema.py` (delegated via subprocess); this C surface validates
 * compatibility metadata, invokes the subprocess via the runner's exec
 * hook, and surfaces a structured error on shape/rank/base-model mismatch.
 *
 * Soundness contract (design §1 / Risk #1):
 *   slow_current and fast MUST share rank, lora_alpha, target modules,
 *   and base-model hash. Mismatch → return HU_ERR_TOOL_VALIDATION with
 *   `out_reason` populated (Sprint 11 / US-11.8 critic-MED #1: header
 *   previously said HU_ERR_PRECONDITION, which does not exist in
 *   `human/core/error.h`; the implementation has always returned
 *   HU_ERR_TOOL_VALIDATION with a discriminator reason string) and emit
 *   `lora_retrain_ema_skipped` (caller's responsibility); do NOT silently
 *   truncate or zero-pad.
 *
 * Cold-start contract (design §1):
 *   When `slow_path_in` does not exist (first night or post-prune), the
 *   helper performs `slow_new := fast` (file copy of `fast_path` to
 *   `slow_path_out`) and returns HU_OK with `out_was_cold_start = 1`. The
 *   alpha value is recorded but is not numerically applied (no prior to
 *   blend against). */

#include "human/core/error.h"
#include "human/ml/lora_retrain_runner.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default EMA alpha — slow_new = alpha * slow + (1 - alpha) * fast.
 * Per OFS-DPO arXiv 2406.05534 + sota-continual.md §3. */
#define HU_LORA_EMA_DEFAULT_ALPHA 0.95

/* Maximum tolerated KL drift, in nats, on the 200-prompt probe set
 * (design §1 KL drift gate). Above this, the EMA result is rejected
 * and `slow_current` is preserved. */
#define HU_LORA_KL_TAU_DEFAULT_NATS 0.5

/* Maximum tolerated NLL regression on the OLD-pairs holdout. Negative
 * deltas indicate forgetting (model assigns lower log-prob to known-good
 * continuations). -0.02 = 2% NLL regression tolerated; deeper trips the
 * forgetting reject (design §1 OLD-pairs). */
#define HU_LORA_FORGET_TAU_NLL_DEFAULT -0.02

/* Subprocess seam — when set on the EMA helper context, replaces real
 * exec. Mirrors `hu_lora_retrain_subprocess_fn` so the runner's existing
 * test hook is directly reusable. */
typedef hu_error_t (*hu_lora_ema_subprocess_fn)(const char *const argv[],
                                                hu_lora_retrain_proc_result_t *result,
                                                void *user_data);

/* EMA invocation context. All paths are NUL-terminated strings. */
typedef struct hu_lora_ema_ctx {
    const char *slow_path_in;  /* prior slow.safetensors.v{N}, or "" for cold-start */
    const char *fast_path;     /* tonight's fast.safetensors */
    const char *slow_path_out; /* destination for slow.safetensors.v{N+1} */
    double alpha;              /* EMA weight on the prior slow (default 0.95) */
    const char *script_path;   /* override "scripts/lora_ema.py" */

    /* Subprocess seam (test hook). When NULL, the EMA helper expects
     * the caller to invoke the subprocess directly — but for the W14
     * runner integration, this is always set to the runner's hook. */
    hu_lora_ema_subprocess_fn run_subprocess;
    void *run_subprocess_ud;

    /* Out — populated on return. */
    int out_was_cold_start; /* 1 iff slow_path_in did not exist on entry */
    char out_reason[128];   /* compat-check failure reason on PRECONDITION */
} hu_lora_ema_ctx_t;

/* Compute slow_new = alpha * slow + (1 - alpha) * fast via the Python
 * helper. Validates compat (rank/target-modules/base-model) before
 * spawning. On cold start, copies fast → slow_out and sets
 * out_was_cold_start = 1.
 *
 * Returns:
 *   HU_OK                    on success
 *   HU_ERR_TOOL_VALIDATION   on rank/target/base-model mismatch
 *                            (out_reason populated; treat as
 *                            "precondition failed" per design §1)
 *   HU_ERR_IO                on subprocess failure (out_reason populated)
 *   HU_ERR_INVALID_ARGUMENT  on NULL ctx / paths */
hu_error_t hu_lora_ema_apply(hu_lora_ema_ctx_t *ctx);

/* KL drift computation. Spawns `scripts/compute_kl_drift.py
 * --base <base> --candidate <candidate_lora> --probe-set <probe.jsonl>`.
 * Parses `{"kl_nats": <float>, "n_prompts": <int>, "source": "real"|"stub"}`
 * from stdout.
 *
 * Sprint 11 / US-11.8 critic-CRITICAL #1 fix: the script returns
 * `source: "stub"` and `kl_nats: 0.0` whenever torch is unavailable
 * (every production deployment until the M3 frontier bridge lands).
 * Previously the C side parsed only `kl_nats`, so a stubbed 0.0 always
 * satisfied the `kl > tau` gate as "clean" — silently turning the KL
 * safety gate into a no-op. Callers MUST inspect `out_is_stub` and
 * treat `1` as "gate not run" (sentinel `last_kl_drift_nats = -1.0`,
 * emit `lora_retrain_kl_gate_stubbed`) — never as a clean PASS.
 *
 * Returns:
 *   HU_OK with *out_kl_nats populated on success
 *     - *out_is_stub = 1 when the script reported `source: "stub"`
 *     - *out_is_stub = 0 on real KL measurement
 *   HU_ERR_IO on subprocess failure or unparseable output
 *
 * `out_is_stub` may be NULL when the caller doesn't care (tests). */
hu_error_t hu_lora_compute_kl_drift(const char *base_path, const char *candidate_path,
                                    const char *probe_set_path, const char *script_path,
                                    hu_lora_ema_subprocess_fn run_subprocess, void *ud,
                                    double *out_kl_nats, int *out_is_stub);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_LORA_EMA_H */
