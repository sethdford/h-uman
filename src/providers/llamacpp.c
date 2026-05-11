/*
 * llama.cpp provider — frontier-model bridge with chat-time LoRA merging.
 *
 * Today this file is the adapter boundary, not the full bridge stack. When
 * `HU_ENABLE_LLAMACPP` is undefined (the default) every vtable method
 * returns HU_ERR_NOT_SUPPORTED. Selecting `llamacpp` from config or
 * the W13 personalization auto-load path will see that, log a benign
 * info-level message, and fall through to the base provider.
 *
 * When the CMake option `HU_ENABLE_LLAMACPP=ON` is set AND `llama.h` is
 * reachable (vendored or system), the body of each `#if HU_LLAMACPP_LINKED`
 * block flips on. We target the **modern llama.cpp API (b3000+)**:
 *
 *   - `llama_model_load_from_file`  — model load (the deprecated
 *     `llama_load_model_from_file` is a -Werror=deprecated trap).
 *   - `llama_init_from_model` — context init (was
 *     `llama_new_context_with_model`).
 *   - `llama_decode` / `llama_sampler_*` — chat (still TODO; chat path is
 *     a NOT_SUPPORTED stub on purpose so the linked build compiles cleanly
 *     before the real tokenize/sample loop lands).
 *   - `llama_adapter_lora_init` + `llama_set_adapters_lora` —
 *     chat-time LoRA merge. Removing the active adapter is done by
 *     calling `llama_set_adapters_lora(ctx, NULL, 0, NULL)` (the modern
 *     API has no single per-adapter remove hook).
 *   - `llama_model_free` — deinit (was `llama_free_model`).
 *
 * If you are linking against an older libllama and these names break the
 * build, vendor a recent upstream under `third_party/llama.cpp/` instead
 * of trying to dual-target the old pre-b3000 spelling.
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
     * live here. Names mirror upstream's modern (b3000+) API. */
    struct llama_model *model;
    struct llama_context *ctx;
    struct llama_adapter_lora *active_adapter;
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
        /* Modern API has no per-adapter remove; clear by setting an empty
         * adapter array on the context. Then free the heap object. */
        (void)llama_set_adapters_lora(c->ctx, NULL, 0, NULL);
        llama_adapter_lora_free(c->active_adapter);
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

/* Multi-message entry point used by `hu_agent_turn` / constitutional /
 * degradation. Delegates to `chat_with_system` using the last system and
 * last user message in the request (same contract as `huml_chat`). */
static hu_error_t llamacpp_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                                const char *model, size_t model_len, double temperature,
                                hu_chat_response_t *out) {
    if (!ctx || !alloc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    const char *sys = NULL;
    size_t sys_len = 0;
    const char *msg = NULL;
    size_t msg_len = 0;

    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == HU_ROLE_SYSTEM) {
            sys = request->messages[i].content;
            sys_len = request->messages[i].content_len;
        } else if (request->messages[i].role == HU_ROLE_USER) {
            msg = request->messages[i].content;
            msg_len = request->messages[i].content_len;
        }
    }

    char *content = NULL;
    size_t content_len = 0;
    hu_error_t err = llamacpp_chat_with_system(ctx, alloc, sys, sys_len, msg, msg_len, model,
                                               model_len, temperature, &content, &content_len);
    out->content = content;
    out->content_len = content_len;
    return err;
}

static bool llamacpp_supports_streaming(void *ctx) {
    (void)ctx;
    return false;
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
    struct llama_adapter_lora *adapter =
        llama_adapter_lora_init(c->model, path_buf);
    if (!adapter) {
        alloc->free(alloc->ctx, path_buf, adapter_path_len + 1);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    /* Modern API: hand the context an array of (adapter, scale) pairs.
     * One adapter at scale 1.0 matches the old per-adapter set call. */
    float scale = 1.0f;
    struct llama_adapter_lora *adapters[1] = {adapter};
    if (llama_set_adapters_lora(c->ctx, adapters, 1, &scale) != 0) {
        llama_adapter_lora_free(adapter);
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
     * load_adapter() picks up the cleanup. Modern API uses an empty
     * adapter array to clear instead of a per-adapter remove call. */
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)ctx;
    if (c->ctx && c->active_adapter) {
        (void)llama_set_adapters_lora(c->ctx, NULL, 0, NULL);
        llama_adapter_lora_free(c->active_adapter);
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
        llama_model_free(c->model);
        c->model = NULL;
    }
#endif
    alloc->free(alloc->ctx, c, sizeof(llamacpp_ctx_t));
}

static const hu_provider_vtable_t llamacpp_vtable = {
    .chat_with_system = llamacpp_chat_with_system,
    .chat = llamacpp_chat,
    .get_name = llamacpp_get_name,
    .supports_native_tools = llamacpp_supports_native_tools,
    .supports_streaming = llamacpp_supports_streaming,
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
     * every turn. The init/free pair is symmetric with deinit. Names
     * use the modern (b3000+) llama.cpp API; the deprecated
     * `llama_load_model_from_file` / `llama_new_context_with_model` are
     * -Werror=deprecated traps under recent libllama. */
    if (c->model_path_owned) {
        struct llama_model_params mp = llama_model_default_params();
        if (config->use_gpu)
            mp.n_gpu_layers = config->n_gpu_layers > 0 ? config->n_gpu_layers : 999;
        c->model = llama_model_load_from_file(c->model_path_owned, mp);
        if (c->model) {
            struct llama_context_params cp = llama_context_default_params();
            if (config->context_size > 0)
                cp.n_ctx = (uint32_t)config->context_size;
            if (config->threads > 0)
                cp.n_threads = (uint32_t)config->threads;
            c->ctx = llama_init_from_model(c->model, cp);
        }
    }
#endif

    out->ctx = c;
    out->vtable = &llamacpp_vtable;
    return HU_OK;
}
