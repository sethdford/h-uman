#ifndef HU_ML_MLX_ADMIN_H
#define HU_ML_MLX_ADMIN_H

/* MLX server admin operations.
 *
 * These helpers talk to the local MLX server's admin endpoints (see
 * scripts/mlx-server.py adapter-swap addition) over HTTP. They are
 * deliberately NOT part of the provider vtable: the `mlx` provider
 * remains a NOT_SUPPORTED stub until/unless we do direct libmlx
 * integration. The admin helpers are loop-closers — they let the
 * daemon swap LoRA adapters at runtime so trained personalization
 * weights can take effect without restarting the MLX server.
 *
 * Phase B2 (2026-05-17 round 2): closes the WRITE side of the M3
 * personalization loop. The READ side (outcome ring buffer) is in
 * `human/ml/m3_frontier_adapter.h`. Together they let a training
 * loop pull recent outcomes, train a new adapter, and swap it in
 * without daemon restart.
 *
 * All functions are HTTP-backed. When `HU_ENABLE_CURL` is OFF, the
 * functions return HU_ERR_NOT_SUPPORTED cleanly — matching the M3
 * dispatcher safety contract. */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_mlx_admin_swap_result {
    /* HTTP status code from the server (200 = ok, 400 = bad request,
     * 404 = adapter path missing, 500 = swap failed mid-apply, etc.).
     * 0 means the request never completed (network error). */
    long status_code;
    /* Number of tensors the server reports it applied (from the
     * response body). 0 when status != 200 or when the response was
     * not parseable. */
    size_t tensors_loaded;
    /* Server-canonical resolved absolute path of the now-active
     * adapter. Owned; free with hu_mlx_admin_swap_result_free. May be
     * NULL on error or if the server didn't return the field. */
    char *resolved_adapter_path;
    size_t resolved_adapter_path_len;
} hu_mlx_admin_swap_result_t;

/* Free the allocator-owned fields inside a swap result. Safe to call
 * with a zero-initialised struct (no double-free risk). */
void hu_mlx_admin_swap_result_free(hu_allocator_t *alloc, hu_mlx_admin_swap_result_t *result);

/* POST {adapter_path: ...} to <base_url>/adapters/swap.
 *
 * `base_url` should point at the MLX server's v1 root (e.g.
 * "http://127.0.0.1:8741/v1"). The "/adapters/swap" suffix is appended
 * by this function.
 *
 * `adapter_path` is the absolute (or ~-expandable, server-side) path
 * of a directory containing `adapters.safetensors`.
 *
 * Returns:
 *   HU_OK on a successful round-trip — even if status_code != 200.
 *     Callers MUST inspect `result->status_code` to distinguish
 *     "swap succeeded" from "swap rejected by server" (e.g. 404 for
 *     missing path).
 *   HU_ERR_NOT_SUPPORTED if libcurl is not linked.
 *   HU_ERR_INVALID_ARGUMENT on NULL inputs.
 *   HU_ERR_IO on transport failure (could not reach the server).
 *   HU_ERR_PROVIDER_RESPONSE on malformed response body. */
hu_error_t hu_mlx_admin_swap_adapter(hu_allocator_t *alloc, const char *base_url,
                                     size_t base_url_len, const char *adapter_path,
                                     size_t adapter_path_len, hu_mlx_admin_swap_result_t *result);

typedef struct hu_mlx_admin_current_adapter {
    /* Same shape rule as swap_result.status_code. */
    long status_code;
    size_t tensors_loaded;
    /* Server-canonical resolved absolute path of the active adapter,
     * or NULL if no adapter is currently applied. Owned; free with
     * hu_mlx_admin_current_adapter_free. */
    char *adapter_path;
    size_t adapter_path_len;
} hu_mlx_admin_current_adapter_t;

void hu_mlx_admin_current_adapter_free(hu_allocator_t *alloc,
                                       hu_mlx_admin_current_adapter_t *current);

/* GET <base_url>/adapters/current. Same return-value contract as
 * swap_adapter: HU_OK on a successful round-trip; status_code holds
 * the server response code. */
hu_error_t hu_mlx_admin_current_adapter(hu_allocator_t *alloc, const char *base_url,
                                        size_t base_url_len, hu_mlx_admin_current_adapter_t *out);

/* ─────────────────────────────────────────────────────────────────────
 * Spec 1 Task 5 (AC-M3-3): swap-failure observability.
 *
 * Every time hu_mlx_admin_swap_adapter() fails (transport, HTTP non-2xx,
 * server-side reject), it:
 *   (a) emits a structured log line at error level via hu_log_error
 *       (adapter_path + status_code + reason label),
 *   (b) increments the appropriate per-reason counter below,
 *   (c) on the FIRST failure post-startup, emits hu_log_info_once with
 *       the "m3 adapter swap path is failing" landmark per
 *       silent-config-gated-subsystems.md.
 *
 * The codebase has no formal metrics-counter library yet; counters are
 * process-global atomic uint64 with public read APIs. Tests read the
 * counters directly; future metrics-export gateway endpoints can do
 * the same.
 * ───────────────────────────────────────────────────────────────── */

typedef enum hu_mlx_swap_failure_reason {
    /* Successful swap (200 OK) — never counted, present so callers can
     * map any classifier to a single enum. */
    HU_MLX_SWAP_FAILURE_NONE = 0,
    /* Could not reach the server (DNS, connection refused, timeout). */
    HU_MLX_SWAP_FAILURE_TRANSPORT = 1,
    /* Server returned a 4xx (other than 404). */
    HU_MLX_SWAP_FAILURE_HTTP_4XX = 2,
    /* Server returned a 5xx. */
    HU_MLX_SWAP_FAILURE_HTTP_5XX = 3,
    /* 404 — the server is alive but the swap endpoint is absent or the
     * adapter path is unknown to the server. Distinguished from generic
     * 4xx because it carries an explicit "operator hasn't deployed the
     * endpoint" signal. */
    HU_MLX_SWAP_FAILURE_MISSING_ENDPOINT = 4,
    /* Other — HU_OK with an unexpected status (e.g. 3xx redirect that
     * curl followed past, 2xx that isn't 200, etc.). */
    HU_MLX_SWAP_FAILURE_OTHER = 5,
    /* Sentinel — keep last. */
    HU_MLX_SWAP_FAILURE__COUNT = 6,
} hu_mlx_swap_failure_reason_t;

/* Per-reason failure counter. NONE/COUNT return 0 (defensive). */
uint64_t hu_mlx_admin_swap_failure_counter(hu_mlx_swap_failure_reason_t reason);

/* Sum across every non-NONE reason. */
uint64_t hu_mlx_admin_swap_failure_total(void);

/* Pure classifier: given the return value of hu_mlx_admin_swap_adapter
 * and the resulting status_code, decide which failure reason applies.
 * Returns HU_MLX_SWAP_FAILURE_NONE when status_code == 200 and err == HU_OK.
 *
 * Extracted as a public predicate (per security-predicate-extraction.md)
 * so tests can pin every branch of the truth table without spinning up
 * a fake HTTP server. */
hu_mlx_swap_failure_reason_t hu_mlx_admin_classify_swap_failure(hu_error_t err, long status_code);

/* Stable string label for a reason — used in log lines and counter
 * label names. Never NULL; returns "none"/"unknown" sentinels for
 * NONE/COUNT. */
const char *hu_mlx_admin_swap_failure_label(hu_mlx_swap_failure_reason_t reason);

/* Test-only: reset all observability counters AND the once-guard so a
 * test can re-arm the first-failure landmark. Production code MUST NOT
 * call this; it exists to make the once-guard testable. */
void hu_mlx_admin_reset_observability_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_MLX_ADMIN_H */
