/*
 * MLX provider — Apple Silicon frontier-model bridge.
 *
 * See include/human/providers/mlx.h for the contract. Today every chat
 * path returns HU_ERR_NOT_SUPPORTED — the file exists so the M3 Bridge B
 * adapter slot has a registered consumer symbol, not because chat works.
 *
 * When `HU_ENABLE_MLX_PROVIDER` is defined and an MLX runtime is
 * reachable, replace the `#if HU_MLX_LINKED` block bodies with the real
 * subprocess + JSON-parse path. Until then, NOT_SUPPORTED is the
 * correct return — the agent's provider fallback path handles it
 * cleanly (see test_provider_all.c daemon-pattern regression guards).
 */

#include "human/providers/mlx.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef HU_ENABLE_MLX_PROVIDER
#define HU_MLX_LINKED 1
#else
#define HU_MLX_LINKED 0
#endif

typedef struct mlx_ctx {
    /* Owned copies so the caller's config struct can be freed after create. */
    char *model_path_owned;
    char *adapter_path_owned;
    int max_tokens;
} mlx_ctx_t;

static char *dup_with_len(hu_allocator_t *alloc, const char *src, size_t len) {
    if (!src || len == 0)
        return NULL;
    char *out = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!out)
        return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

/* ── vtable: chat ─────────────────────────────────────────────────────── */

static hu_error_t mlx_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                           const char *model, size_t model_len, double temperature,
                           hu_chat_response_t *out) {
    (void)ctx;
    (void)alloc;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)out;
#if HU_MLX_LINKED
    /* TODO Bridge B Phase B.1: shell out to `python3 -m mlx_lm.generate`
     * with the configured model + adapter, parse the JSON response into
     * `out`. */
    return HU_ERR_NOT_SUPPORTED;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t mlx_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                       size_t system_len, const char *message, size_t message_len,
                                       const char *model, size_t model_len, double temperature,
                                       char **out, size_t *out_len) {
    (void)ctx;
    (void)alloc;
    (void)system_prompt;
    (void)system_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}

static bool mlx_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *mlx_get_name(void *ctx) {
    (void)ctx;
    return "mlx";
}

static void mlx_deinit(void *ctx, hu_allocator_t *alloc) {
    mlx_ctx_t *c = (mlx_ctx_t *)ctx;
    if (!c)
        return;
    if (c->model_path_owned)
        alloc->free(alloc->ctx, c->model_path_owned, strlen(c->model_path_owned) + 1);
    if (c->adapter_path_owned)
        alloc->free(alloc->ctx, c->adapter_path_owned, strlen(c->adapter_path_owned) + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

/* ── vtable: load_adapter ─────────────────────────────────────────────── */

static hu_error_t mlx_load_adapter(void *ctx, hu_allocator_t *alloc, const char *adapter_path,
                                   size_t adapter_path_len, const char *adapter_id,
                                   size_t adapter_id_len) {
    (void)ctx;
    (void)alloc;
    (void)adapter_path;
    (void)adapter_path_len;
    (void)adapter_id;
    (void)adapter_id_len;
    /* When linked: validate adapter is safetensors format, store path
     * for the next chat call to pass to mlx_lm.generate via --adapter-path.
     * Until then, NOT_SUPPORTED matches the dispatcher safety contract
     * the daemon expects (see test_provider_all.c). */
    return HU_ERR_NOT_SUPPORTED;
}

static const char *mlx_active_adapter(void *ctx) {
    (void)ctx;
    return NULL;
}

/* ── vtable ───────────────────────────────────────────────────────────── */

static const hu_provider_vtable_t mlx_vtable = {
    .chat = mlx_chat,
    .chat_with_system = mlx_chat_with_system,
    .supports_native_tools = mlx_supports_native_tools,
    .get_name = mlx_get_name,
    .deinit = mlx_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .stream_chat = NULL,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .load_adapter = mlx_load_adapter,
    .active_adapter = mlx_active_adapter,
};

hu_error_t hu_mlx_provider_create(hu_allocator_t *alloc, const hu_mlx_config_t *config,
                                  hu_provider_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    mlx_ctx_t *c = (mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));

    if (config) {
        c->model_path_owned = dup_with_len(alloc, config->model_path, config->model_path_len);
        c->adapter_path_owned = dup_with_len(alloc, config->adapter_path, config->adapter_path_len);
        c->max_tokens = config->max_tokens;
        /* If the caller asked for a model/adapter but allocation failed,
         * surface OOM rather than silently dropping the request. */
        if ((config->model_path && config->model_path_len > 0 && !c->model_path_owned) ||
            (config->adapter_path && config->adapter_path_len > 0 && !c->adapter_path_owned)) {
            mlx_deinit(c, alloc);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    out->ctx = c;
    out->vtable = &mlx_vtable;
    return HU_OK;
}
