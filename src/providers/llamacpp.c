/*
 * llama.cpp provider — frontier-model bridge with chat-time LoRA merging.
 *
 * Today this file is the adapter boundary, not the full bridge stack. When
 * `HU_ENABLE_LLAMACPP` is undefined (the default) every vtable method
 * returns HU_ERR_NOT_SUPPORTED. Selecting `llamacpp` from config or
 * the W13 personalization auto-load path will see that, log a benign
 * info-level message, and fall through to the base provider.
 *
 * Once libllama is vendored under `third_party/llama.cpp/` and the
 * CMake option `HU_ENABLE_LLAMACPP=ON` is set, the body of each
 * `#ifdef HU_ENABLE_LLAMACPP` block flips on and the methods map onto:
 *
 *   - `llama_load_model_from_file`  — model load
 *   - `llama_decode` / `llama_sampler_*` — chat
 *   - `llama_lora_adapter_init_from_file` + `llama_lora_adapter_set`
 *     — chat-time LoRA merge
 *   - `llama_free_model` — deinit
 *
 * See `docs/plans/2026-05-10-m3-frontier-model-bridge.md` Bridge A.
 */

#include "human/providers/llamacpp.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_LLAMACPP
/* Once libllama is vendored, include "llama.h" here. The #error guards
 * against a misconfigured build where the option is on but the library
 * headers are not on the include path. */
#if __has_include("llama.h")
#include "llama.h"
#define HU_LLAMACPP_LINKED 1
#else
#define HU_LLAMACPP_LINKED 0
#endif
#else
#define HU_LLAMACPP_LINKED 0
#endif

typedef struct llamacpp_ctx {
    hu_llamacpp_config_t config;
    /* Owned copy of `config.model_path` so the caller can free its
     * config struct after `create()` returns. */
    char *model_path_owned;

    /* Currently-active adapter id (for the active_adapter hook).
     * Heap-allocated; NULL when nothing is loaded. */
    char *active_adapter_id;
    char *active_adapter_path; /* echo of last load_adapter path for diag */

#if HU_LLAMACPP_LINKED
    /* When we actually link libllama, the live model + context handles
     * live here. Names mirror upstream. */
    struct llama_model *model;
    struct llama_context *ctx;
    struct llama_lora_adapter *active_adapter;
#endif
} llamacpp_ctx_t;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void clear_active_adapter(llamacpp_ctx_t *c, hu_allocator_t *alloc) {
    if (!c)
        return;
    if (c->active_adapter_id) {
        alloc->free(alloc->ctx, c->active_adapter_id,
                    strlen(c->active_adapter_id) + 1);
        c->active_adapter_id = NULL;
    }
    if (c->active_adapter_path) {
        alloc->free(alloc->ctx, c->active_adapter_path,
                    strlen(c->active_adapter_path) + 1);
        c->active_adapter_path = NULL;
    }
#if HU_LLAMACPP_LINKED
    if (c->ctx && c->active_adapter) {
        llama_lora_adapter_remove(c->ctx, c->active_adapter);
        llama_lora_adapter_free(c->active_adapter);
        c->active_adapter = NULL;
    }
#endif
}

/* ── vtable: chat ─────────────────────────────────────────────────────── */

static hu_error_t llamacpp_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                            const char *system_prompt,
                                            size_t system_prompt_len,
                                            const char *message, size_t message_len,
                                            const char *model, size_t model_len,
                                            double temperature, char **out,
                                            size_t *out_len) {
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)alloc;
    (void)out;
    (void)out_len;
#if HU_LLAMACPP_LINKED
    /* Real llama.cpp chat path lives here once vendored:
     *   tokens = llama_tokenize(...)
     *   batch  = llama_batch_init(...)
     *   for each step:
     *     llama_decode(c->ctx, batch)
     *     sample greedy / top-k / top-p with temperature
     *   llama_token_to_piece(...) -> *out
     * For now even the linked build is a stub so the binary builds
     * cleanly without the upstream API drift hitting us. */
    return HU_ERR_NOT_SUPPORTED;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ── vtable: identity / capability hooks ──────────────────────────────── */

static const char *llamacpp_get_name(void *ctx) {
    (void)ctx;
    return "llamacpp";
}

static bool llamacpp_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

/* ── vtable: LoRA adapter management ──────────────────────────────────── */

static hu_error_t llamacpp_load_adapter(void *ctx, hu_allocator_t *alloc,
                                        const char *adapter_path,
                                        size_t adapter_path_len,
                                        const char *adapter_id,
                                        size_t adapter_id_len) {
    if (!ctx || !alloc || !adapter_path || adapter_path_len == 0 ||
        !adapter_id || adapter_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)ctx;
#if HU_LLAMACPP_LINKED
    if (!c->ctx || !c->model)
        return HU_ERR_NOT_SUPPORTED;
    /* Replace any active adapter atomically so callers see at-most-one
     * adapter active at any time. */
    clear_active_adapter(c, alloc);
    char *path_buf = alloc->alloc(alloc->ctx, adapter_path_len + 1);
    if (!path_buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(path_buf, adapter_path, adapter_path_len);
    path_buf[adapter_path_len] = '\0';
    struct llama_lora_adapter *adapter =
        llama_lora_adapter_init_from_file(c->model, path_buf);
    if (!adapter) {
        alloc->free(alloc->ctx, path_buf, adapter_path_len + 1);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    if (llama_lora_adapter_set(c->ctx, adapter, 1.0f) != 0) {
        llama_lora_adapter_free(adapter);
        alloc->free(alloc->ctx, path_buf, adapter_path_len + 1);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    c->active_adapter = adapter;
    c->active_adapter_path = path_buf;
    c->active_adapter_id = hu_strndup(alloc, adapter_id, adapter_id_len);
    if (!c->active_adapter_id) {
        clear_active_adapter(c, alloc);
        return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
#else
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t llamacpp_unload_adapter(void *ctx, const char *adapter_id,
                                          size_t adapter_id_len) {
    if (!ctx)
        return HU_ERR_INVALID_ARGUMENT;
    (void)adapter_id;
    (void)adapter_id_len;
#if HU_LLAMACPP_LINKED
    /* Without an allocator on this hook we cannot free the cached id,
     * so we just zero the live adapter. The next replace cycle in
     * load_adapter() picks up the cleanup. */
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)ctx;
    if (c->ctx && c->active_adapter) {
        llama_lora_adapter_remove(c->ctx, c->active_adapter);
        llama_lora_adapter_free(c->active_adapter);
        c->active_adapter = NULL;
    }
    return HU_OK;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static const char *llamacpp_active_adapter(void *ctx) {
    if (!ctx)
        return NULL;
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)ctx;
    return c->active_adapter_id;
}

/* ── vtable: deinit ───────────────────────────────────────────────────── */

static void llamacpp_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)ctx;
    clear_active_adapter(c, alloc);
    if (c->model_path_owned) {
        alloc->free(alloc->ctx, c->model_path_owned, strlen(c->model_path_owned) + 1);
        c->model_path_owned = NULL;
    }
#if HU_LLAMACPP_LINKED
    if (c->ctx) {
        llama_free(c->ctx);
        c->ctx = NULL;
    }
    if (c->model) {
        llama_free_model(c->model);
        c->model = NULL;
    }
#endif
    alloc->free(alloc->ctx, c, sizeof(llamacpp_ctx_t));
}

static const hu_provider_vtable_t llamacpp_vtable = {
    .chat_with_system = llamacpp_chat_with_system,
    .chat = NULL,
    .get_name = llamacpp_get_name,
    .supports_native_tools = llamacpp_supports_native_tools,
    .load_adapter = llamacpp_load_adapter,
    .unload_adapter = llamacpp_unload_adapter,
    .active_adapter = llamacpp_active_adapter,
    .deinit = llamacpp_deinit,
};

/* ── factory ──────────────────────────────────────────────────────────── */

hu_error_t hu_llamacpp_provider_create(hu_allocator_t *alloc,
                                       const hu_llamacpp_config_t *config,
                                       hu_provider_t *out) {
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;
    llamacpp_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(llamacpp_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->config = *config;
    c->config.model_path = NULL; /* don't borrow caller's pointer */

    if (config->model_path && *config->model_path) {
        c->model_path_owned = hu_strdup(alloc, config->model_path);
        if (!c->model_path_owned) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

#if HU_LLAMACPP_LINKED
    /* Eagerly load the model so chat() doesn't pay startup cost on
     * every turn. The init/free pair is symmetric with deinit. */
    if (c->model_path_owned) {
        struct llama_model_params mp = llama_model_default_params();
        if (config->use_gpu)
            mp.n_gpu_layers = config->n_gpu_layers > 0 ? config->n_gpu_layers : 999;
        c->model = llama_load_model_from_file(c->model_path_owned, mp);
        if (c->model) {
            struct llama_context_params cp = llama_context_default_params();
            if (config->context_size > 0)
                cp.n_ctx = (uint32_t)config->context_size;
            if (config->threads > 0)
                cp.n_threads = (uint32_t)config->threads;
            c->ctx = llama_new_context_with_model(c->model, cp);
        }
    }
#endif

    out->ctx = c;
    out->vtable = &llamacpp_vtable;
    return HU_OK;
}
