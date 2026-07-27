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
#include "human/core/endpoints.h"
#include "human/core/error.h"

#include <stdbool.h>
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
 * HU_MLX_DEFAULT_BASE_URL). The "/adapters/swap" suffix is appended
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

/* ─────────────────────────────────────────────────────────────────────
 * LoRA scale safety gate (per .claude/rules/lora-scale-default-or-die.md).
 *
 * LoRA scale is baked into BOTH training and fusion; a scale that is too
 * high over-amplifies the adapter delta and collapses the base model's
 * instruction-following (the "zero usable replies for 2 weeks" failure).
 * The rule mandates scale=2.0 and REJECTS anything >8.0. These predicates
 * let the serve/swap boundary refuse a dangerously over-scaled adapter,
 * and are pure so tests pin every branch without a model. */
typedef enum {
    HU_LORA_SCALE_SAFE = 0, /* scale <= 4.0 */
    HU_LORA_SCALE_WARN,     /* 4.0 < scale <= 8.0 — validate before fusion */
    HU_LORA_SCALE_REJECT,   /* scale > 8.0 — refuse */
} hu_lora_scale_class_t;

hu_lora_scale_class_t hu_lora_scale_classify(double scale);

/* Read lora_parameters.scale (or top-level scale) from
 * <adapter_dir>/adapter_config.json. Returns HU_OK + *out_scale on
 * success; HU_ERR_NOT_FOUND when the file or field is absent/unreadable
 * (caller decides fail-open). */
hu_error_t hu_lora_adapter_config_scale(hu_allocator_t *alloc, const char *adapter_dir,
                                        size_t adapter_dir_len, double *out_scale);

/* Serve/swap-boundary guard: HU_OK if the adapter at adapter_path is safe
 * to serve, HU_ERR_INVALID_ARGUMENT if its scale classifies as REJECT
 * (>8.0). Fail-open (HU_OK) when no readable adapter_config.json — matches
 * the swap path's existing fail-open posture. */
hu_error_t hu_lora_scale_guard_serveable(hu_allocator_t *alloc, const char *adapter_path,
                                         size_t adapter_path_len);

/* Test-only: reset all observability counters AND the once-guard so a
 * test can re-arm the first-failure landmark. Production code MUST NOT
 * call this; it exists to make the once-guard testable. */
void hu_mlx_admin_reset_observability_for_test(void);

/* ─────────────────────────────────────────────────────────────────────
 * Local-MLX health probe (2026-05-27 Dermot humanness recovery C2).
 *
 * Lets the model router gate Seth-voice mlx_local routing on the local
 * server actually being reachable. GETs <base_url>/adapters/current (the
 * same v1 root the swap infra targets) and treats HTTP 200 as healthy.
 * The result is cached for 60s so the hot per-turn route path issues at
 * most one HTTP round-trip per minute.
 *
 * Returns false (→ caller routes to cloud) on any error, NULL args, or in
 * a non-curl build. The router stays pure: it consumes the bool this
 * returns via cfg.mlx_local_healthy; it never calls this itself.
 *
 * TIMEOUT CAVEAT: hu_http_get uses the default (~30s) timeout — there is
 * no bounded-timeout GET helper yet. A dead server therefore stalls the
 * FIRST probe of each 60s window up to that bound. mlx_local routing is
 * operator opt-in (the operator is running the server), so this is
 * acceptable for v1; adding a bounded-timeout GET is a tracked follow-up. */
bool hu_mlx_admin_probe_health(hu_allocator_t *alloc, const char *base_url, size_t base_url_len);

/* Test-only: force the probe result without touching the network, and
 * clear the override (also clears the 60s cache). Honored in both curl
 * and non-curl builds. Mirrors hu_mlx_admin_reset_observability_for_test. */
void hu_mlx_admin_set_test_health(bool healthy);
void hu_mlx_admin_clear_test_health(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_MLX_ADMIN_H */
