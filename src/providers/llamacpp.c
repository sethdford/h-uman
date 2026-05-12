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
#include "human/providers/llamacpp_decode.h"
#include "human/providers/llamacpp_kvcache.h"
#include "human/providers/llamacpp_sampling.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

    /* Phase 1 (RL SOTA) — system-prompt KV-cache index. Tracks the
     * (hash, n_past) of the last decoded system prefix so a subsequent
     * call with the same system prompt skips re-decoding it. Adapter
     * load/unload MUST reset this because per-token KV depends on
     * effective weights. The actual KV memory lives inside ctx. */
    hu_llamacpp_kvcache_t kv_cache;
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

#if HU_LLAMACPP_LINKED

/* Phase 1 (RL SOTA) — Gemma-3 chat-template renderer.
 *
 * Gemma-3 uses the same `<start_of_turn>` / `<end_of_turn>` literals
 * the chat template baked into the GGUF expects. The 192-byte margin
 * over the message size is critic-pinned: earlier drafts used 64
 * bytes which truncated the template literals on long system prompts.
 *
 * Buffer is malloc'd because the prompt can grow arbitrarily large
 * with system + message; the caller frees on every exit path.
 */
static char *llamacpp_render_template(const char *system_prompt,
                                      size_t system_prompt_len,
                                      const char *message,
                                      size_t message_len,
                                      size_t *out_len) {
    static const char OPEN_SYS[]   = "<start_of_turn>system\n";
    static const char CLOSE_TURN[] = "<end_of_turn>\n";
    static const char OPEN_USER[]  = "<start_of_turn>user\n";
    static const char OPEN_MODEL[] = "<start_of_turn>model\n";
    /* 192-byte template-literal margin; +1 for NUL. */
    size_t combined_cap = system_prompt_len + message_len + 192 + 1;
    char *buf = (char *)malloc(combined_cap);
    if (!buf) return NULL;
    int n = snprintf(buf, combined_cap, "%s%.*s%s%s%.*s%s%s",
                     OPEN_SYS,
                     (int)system_prompt_len, system_prompt ? system_prompt : "",
                     CLOSE_TURN,
                     OPEN_USER,
                     (int)message_len, message ? message : "",
                     CLOSE_TURN,
                     OPEN_MODEL);
    if (n < 0 || (size_t)n >= combined_cap) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)n;
    return buf;
}

/* Bridge our hu_llamacpp_logits_fn into llama_get_logits_ith. */
static hu_error_t llamacpp_real_logits(void *ctx_ptr, size_t batch_pos,
                                       float *out_logits, size_t vocab_size) {
    (void)batch_pos;
    struct llama_context *ctx = (struct llama_context *)ctx_ptr;
    const float *src = llama_get_logits_ith(ctx, -1);
    if (!src) return HU_ERR_PROVIDER_RESPONSE;
    memcpy(out_logits, src, sizeof(float) * vocab_size);
    return HU_OK;
}

/* Bridge our hu_llamacpp_advance_fn into llama_decode of one token.
 * Without this the next call to llama_get_logits_ith returns the
 * frozen distribution from the prefix decode and the loop emits the
 * same token forever (Critic Blocker 1). */
static hu_error_t llamacpp_real_advance(void *ctx_ptr, int32_t token) {
    struct llama_context *ctx = (struct llama_context *)ctx_ptr;
    llama_token tok = (llama_token)token;
    if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0)
        return HU_ERR_PROVIDER_RESPONSE;
    return HU_OK;
}

#endif /* HU_LLAMACPP_LINKED */

static hu_error_t llamacpp_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                            const char *system_prompt,
                                            size_t system_prompt_len,
                                            const char *message, size_t message_len,
                                            const char *model, size_t model_len,
                                            double temperature, char **out,
                                            size_t *out_len) {
    if (!ctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    (void)model;
    (void)model_len;
#if HU_LLAMACPP_LINKED
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)ctx;
    if (!c->ctx || !c->model) return HU_ERR_NOT_SUPPORTED;

    const struct llama_vocab *vocab = llama_model_get_vocab(c->model);
    if (!vocab) return HU_ERR_PROVIDER_RESPONSE;
    int32_t vocab_n = llama_vocab_n_tokens(vocab);
    if (vocab_n <= 0) return HU_ERR_PROVIDER_RESPONSE;
    llama_token eos = llama_vocab_eos(vocab);
    llama_token eot = llama_vocab_eot(vocab);

    /* ── 1. Render the chat template ──────────────────────────────── */
    size_t prompt_len = 0;
    char *prompt = llamacpp_render_template(system_prompt, system_prompt_len,
                                            message, message_len, &prompt_len);
    if (!prompt) return HU_ERR_OUT_OF_MEMORY;

    /* ── 2. Tokenize the rendered prompt ─────────────────────────── */
    /* First call returns negative whose absolute value is the number
     * of tokens needed; allocate that, then call again. */
    int32_t n_probe = llama_tokenize(vocab, prompt, (int32_t)prompt_len,
                                     NULL, 0, /*add_special=*/true,
                                     /*parse_special=*/true);
    int32_t n_tokens_needed = n_probe < 0 ? -n_probe : n_probe;
    if (n_tokens_needed <= 0) {
        free(prompt);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    llama_token *tokens = (llama_token *)malloc(sizeof(llama_token) * (size_t)n_tokens_needed);
    if (!tokens) {
        free(prompt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int32_t n_tokens = llama_tokenize(vocab, prompt, (int32_t)prompt_len,
                                      tokens, n_tokens_needed,
                                      /*add_special=*/true, /*parse_special=*/true);
    free(prompt);
    if (n_tokens <= 0) { free(tokens); return HU_ERR_PROVIDER_RESPONSE; }

    /* ── 3. KV cache: lookup, decode prefix, record ─────────────── */
    /* Phase 1 keeps this simple: clear the KV cache for any miss
     * (full re-decode of the new prompt). The hit path is a no-op
     * fast-path for the common "same system prompt, new user msg"
     * pattern in agent loops; on a hit we still re-decode the user
     * portion because we don't track its position separately yet. */
    int32_t cached_n_past = 0;
    bool sys_hit =
        (hu_llamacpp_kvcache_lookup_system(&c->kv_cache, system_prompt,
                                           system_prompt_len, &cached_n_past) == HU_OK);
    if (!sys_hit) {
        llama_memory_clear(llama_get_memory(c->ctx), /*data=*/true);
    }
    /* Decode the full prompt as one batch (Phase 1 simplification —
     * the prefix-skip optimization is Phase 3+). */
    if (llama_decode(c->ctx, llama_batch_get_one(tokens, n_tokens)) != 0) {
        free(tokens);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    /* Record how far we got so the next call sees the same system
     * hash and can short-circuit (today: still re-decodes; the
     * hash-hit signal is the foundation for Phase 3+). */
    if (system_prompt && system_prompt_len > 0) {
        (void)hu_llamacpp_kvcache_record_system(&c->kv_cache, system_prompt,
                                                system_prompt_len, n_tokens);
    }

    /* ── 4. Sampler + decode loop ───────────────────────────────── */
    hu_llamacpp_sampling_params_t sparams = {
        .temperature = temperature,
        .top_k = 40,
        .top_p = 0.95,
        .min_p = 0.05,
        /* Greedy (temperature == 0) must be deterministic; use a fixed
         * non-zero seed (the sampler's "0 -> system random" fallback
         * would defeat reproducibility). The parenthesized comparison
         * fixes the precedence bug from earlier drafts where a cast
         * outside the parens turned every fractional temperature into
         * "== 0.0" -> seed 1 -> always-deterministic. */
        .seed = (temperature == 0.0) ? (uint64_t)1 : (uint64_t)time(NULL),
    };
    hu_llamacpp_sampler_t sampler = {0};
    if (hu_llamacpp_sampler_init(&sampler, &sparams) != HU_OK) {
        free(tokens);
        return HU_ERR_OUT_OF_MEMORY;
    }

    enum { MAX_OUT_TOKENS = 512 };
    llama_token *sampled = (llama_token *)malloc(sizeof(llama_token) * MAX_OUT_TOKENS);
    if (!sampled) {
        hu_llamacpp_sampler_free(&sampler);
        free(tokens);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t sampled_len = 0;
    /* Decode loop: stop at either EOS or the chat-end-of-turn token.
     * Gemma chat templates emit EOT to close the turn before EOS, so
     * we treat EOT as the canonical halt signal. */
    hu_llamacpp_decode_config_t dcfg = {
        .max_tokens = MAX_OUT_TOKENS,
        .eos_token = eot,
        .vocab_size = (size_t)vocab_n,
        .logits_provider = llamacpp_real_logits,
        .logits_ctx = c->ctx,
        .advance = llamacpp_real_advance,
        .advance_ctx = c->ctx,
        .sampler = &sampler,
    };
    hu_error_t derr = hu_llamacpp_decode_run(&dcfg, sampled, &sampled_len);
    /* If EOT wasn't produced but EOS was, the decode loop ended at
     * the right place — both are acceptable terminators. */
    (void)eos;
    if (derr != HU_OK && sampled_len == 0) {
        free(sampled);
        free(tokens);
        hu_llamacpp_sampler_free(&sampler);
        return derr;
    }

    /* ── 5. Detokenize into a heap string for the caller ────────── */
    /* Probe required size (negative == "needs this many bytes"). */
    int32_t need = llama_detokenize(vocab, sampled, (int32_t)sampled_len,
                                    NULL, 0, /*remove_special=*/false,
                                    /*unparse_special=*/false);
    int32_t need_abs = need < 0 ? -need : need;
    if (need_abs <= 0) need_abs = 1;
    char *result = (char *)alloc->alloc(alloc->ctx, (size_t)need_abs + 1);
    if (!result) {
        free(sampled);
        free(tokens);
        hu_llamacpp_sampler_free(&sampler);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int32_t wrote = llama_detokenize(vocab, sampled, (int32_t)sampled_len,
                                     result, need_abs,
                                     /*remove_special=*/false,
                                     /*unparse_special=*/false);
    if (wrote < 0) wrote = need_abs;
    if (wrote > need_abs) wrote = need_abs;
    result[wrote] = '\0';

    free(sampled);
    free(tokens);
    hu_llamacpp_sampler_free(&sampler);

    *out = result;
    *out_len = (size_t)wrote;
    return HU_OK;
#else
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)temperature;
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
    /* Phase 1 critic-fix: per-token KV depends on the model's effective
     * weights. After swapping in a LoRA, any cached KV from the base
     * model is now stale — wipe both the llama_context's KV memory and
     * our system-prefix index so the next chat call decodes fresh. */
    llama_memory_clear(llama_get_memory(c->ctx), /*data=*/true);
    hu_llamacpp_kvcache_reset(&c->kv_cache);
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
        /* Mirror the load_adapter contract: clearing back to the
         * base model invalidates any cached KV from the adapter run. */
        llama_memory_clear(llama_get_memory(c->ctx), /*data=*/true);
        hu_llamacpp_kvcache_reset(&c->kv_cache);
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
    hu_llamacpp_kvcache_free(&c->kv_cache);
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

/* Phase 1 (RL SOTA) — vtable.warmup hook.
 *
 * The big cost of llamacpp inference on Apple Silicon is the FIRST
 * request: Metal compiles ~150 GPU kernels (visible in stderr as
 * `ggml_metal_library_compile_pipeline: ...`) lazily on the first
 * llama_decode call. That bursts ~700 ms onto the request that
 * happens to land first.
 *
 * Pre-touching memory + the empty-batch decode path forces those
 * kernels to load now, while the user is still inside their setup
 * step. Subsequent chat calls then pay only the actual decode cost.
 *
 * This is a no-op when the model isn't loaded (stub build, missing
 * model_path, or _provider_create failure path). */
static void llamacpp_warmup(void *vctx) {
#if HU_LLAMACPP_LINKED
    llamacpp_ctx_t *c = (llamacpp_ctx_t *)vctx;
    if (!c || !c->ctx || !c->model) return;
    const struct llama_vocab *vocab = llama_model_get_vocab(c->model);
    if (!vocab) return;
    llama_token bos = llama_vocab_bos(vocab);
    if (bos < 0) return;
    /* Decode a single BOS token — this triggers Metal kernel
     * compilation without committing any conversational state. */
    struct llama_batch batch = llama_batch_get_one(&bos, 1);
    (void)llama_decode(c->ctx, batch);
    /* Clear the KV slot we just used so the warmup doesn't leak
     * into subsequent chat calls' system-prompt cache. */
    llama_memory_clear(llama_get_memory(c->ctx), true);
    hu_llamacpp_kvcache_reset(&c->kv_cache);
#else
    (void)vctx;
#endif
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
    .warmup = llamacpp_warmup,
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

    /* Phase 1 (RL SOTA) — KV-cache index starts empty; the first
     * chat call observes a miss, clears the llama_context KV memory,
     * decodes the system prefix, and records the slot. */
    hu_llamacpp_kvcache_init(&c->kv_cache);

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

/* Phase 1 (RL SOTA) — sanity-gate one-shot CLI.
 *
 * Wired from tests/test_main.c when the binary is launched as
 * `human_tests --sanity-gate <gguf-path> <system> <user>`. Loads the
 * GGUF, decodes one chat turn at temperature 0.0, prints the response
 * to stdout, exits.
 *
 * Used by scripts/run-gemma-sanity-gate.sh to score the 20-prompt
 * fixture at tests/fixtures/gemma_sanity_gate_prompts.json.
 */
#if HU_LLAMACPP_LINKED
int hu_llamacpp_sanity_gate_main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s --sanity-gate <gguf-path> <system> <user>\n",
                argv[0]);
        return 2;
    }
    hu_allocator_t alloc = hu_system_allocator();
    hu_llamacpp_config_t cfg = {
        .model_path = argv[2],
        .context_size = 2048,
        .threads = 4,
        .use_gpu = true,
        .n_gpu_layers = -1,
    };
    hu_provider_t provider = {0};
    if (hu_llamacpp_provider_create(&alloc, &cfg, &provider) != HU_OK) {
        fprintf(stderr, "[sanity-gate] failed to create provider for %s\n", argv[2]);
        return 3;
    }
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = provider.vtable->chat_with_system(
        provider.ctx, &alloc, argv[3], strlen(argv[3]), argv[4], strlen(argv[4]),
        "gemma-3-4b-it", strlen("gemma-3-4b-it"), 0.0, &out, &out_len);
    if (err == HU_OK && out) {
        fwrite(out, 1, out_len, stdout);
        fputc('\n', stdout);
        free(out);
    } else {
        fprintf(stderr, "[sanity-gate] chat_with_system returned %d\n", (int)err);
    }
    if (provider.vtable->deinit) provider.vtable->deinit(provider.ctx, &alloc);
    return (err == HU_OK) ? 0 : 4;
}
#else
int hu_llamacpp_sanity_gate_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "[sanity-gate] llama.cpp not linked into this build\n");
    return 1;
}
#endif
