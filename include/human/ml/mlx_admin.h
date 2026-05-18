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

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_MLX_ADMIN_H */
