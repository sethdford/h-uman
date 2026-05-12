/*
 * HUML checkpoint provider — local inference from trained HUML models.
 * Loads a checkpoint saved by hu_ml_checkpoint_save, creates a GPT model,
 * and runs the forward pass for next-token prediction.
 * Gated behind HU_ENABLE_ML.
 */

#include "human/providers/huml.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_ML
#include "human/ml/checkpoint.h"
#include "human/ml/lora.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/tokenizer_ml.h"
#endif

#define HUML_MAX_ADAPTER_ID 64

/* Reference-GPT shape used by the huml provider. Adapters must match
 * these dims to attach into the forward pass; otherwise we keep the
 * adapter in the slot but skip attach + log a one-shot caveat. */
#define HUML_GPT_N_EMBD 512
#define HUML_GPT_N_LAYER 8
#define HUML_GPT_VOCAB 8192
#define HUML_GPT_N_HEAD 4
#define HUML_GPT_HEAD_DIM 128
#define HUML_GPT_SEQ_LEN 2048

typedef struct {
    hu_allocator_t *alloc;
    char checkpoint_path[512];
    size_t max_tokens;
#ifdef HU_ENABLE_ML
    hu_model_t model;
    hu_ml_optimizer_t optimizer;
    hu_bpe_tokenizer_t *tokenizer;
    bool loaded;
    /* W13 — single-adapter slot. Multi-adapter routing arrives in Phase 4
     * of the FIX 15 frontier-bridge plan; for now the contract is "at
     * most one adapter live at a time". */
    hu_lora_adapter_t *active_lora;
    char active_adapter_id[HUML_MAX_ADAPTER_ID];
    bool active_lora_attached;
#endif
} huml_ctx_t;

#ifdef HU_ENABLE_ML
/* Forward declarations for attach helpers — definitions live with the
 * adapter-loading code below to keep all LoRA glue in one block. */
static void huml_try_attach_active_lora(huml_ctx_t *ctx);
static void huml_detach_active_lora(huml_ctx_t *ctx);
#endif

#if defined(HU_ENABLE_ML) && !HU_IS_TEST
static hu_error_t ensure_loaded(huml_ctx_t *ctx) {
    if (ctx->loaded)
        return HU_OK;
    if (ctx->checkpoint_path[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;

    hu_gpt_config_t gpt_cfg = {0};
    gpt_cfg.sequence_len = HUML_GPT_SEQ_LEN;
    gpt_cfg.vocab_size = HUML_GPT_VOCAB;
    gpt_cfg.n_layer = HUML_GPT_N_LAYER;
    gpt_cfg.n_head = HUML_GPT_N_HEAD;
    gpt_cfg.n_kv_head = HUML_GPT_N_HEAD;
    gpt_cfg.n_embd = HUML_GPT_N_EMBD;
    gpt_cfg.head_dim = HUML_GPT_HEAD_DIM;
    memcpy(gpt_cfg.window_pattern, "SSSL", 5);
    gpt_cfg.activation = HU_ML_ACT_RELU_SQ;
    gpt_cfg.logit_soft_cap = 30.0f;

    hu_error_t err = hu_gpt_create(ctx->alloc, &gpt_cfg, &ctx->model);
    if (err != HU_OK)
        return err;

    hu_optimizer_config_t opt_cfg = {0};
    err = hu_muon_adamw_create(ctx->alloc, &opt_cfg, &ctx->optimizer);
    if (err != HU_OK) {
        ctx->model.vtable->deinit(ctx->model.ctx, ctx->alloc);
        return err;
    }

    err = hu_ml_checkpoint_load(ctx->alloc, ctx->checkpoint_path, &ctx->model, &ctx->optimizer);
    if (err != HU_OK) {
        ctx->optimizer.vtable->deinit(ctx->optimizer.ctx, ctx->alloc);
        ctx->model.vtable->deinit(ctx->model.ctx, ctx->alloc);
        return err;
    }

    err = hu_bpe_tokenizer_create(ctx->alloc, &ctx->tokenizer);
    if (err != HU_OK) {
        ctx->optimizer.vtable->deinit(ctx->optimizer.ctx, ctx->alloc);
        ctx->model.vtable->deinit(ctx->model.ctx, ctx->alloc);
        return err;
    }

    ctx->loaded = true;

    /* W13 Phase 4 — lazy attach: if an adapter was loaded before the
     * model, attach it now so the very first forward pass is biased. */
    huml_try_attach_active_lora(ctx);
    return HU_OK;
}

static int32_t sample_argmax(const float *logits, size_t vocab_size) {
    int32_t best = 0;
    float best_val = logits[0];
    for (size_t i = 1; i < vocab_size; i++) {
        if (logits[i] > best_val) {
            best_val = logits[i];
            best = (int32_t)i;
        }
    }
    return best;
}
#endif

static hu_error_t huml_chat_with_system(void *ctx_ptr, hu_allocator_t *alloc,
                                        const char *system_prompt, size_t system_prompt_len,
                                        const char *message, size_t message_len, const char *model,
                                        size_t model_len, double temperature, char **out,
                                        size_t *out_len) {
    (void)model;
    (void)model_len;
    (void)temperature;

    huml_ctx_t *ctx = (huml_ctx_t *)ctx_ptr;
    if (!ctx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    *out = hu_strndup(alloc, "[huml test]", 11);
    *out_len = *out ? 11 : 0;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
#else
#ifdef HU_ENABLE_ML
    hu_error_t err = ensure_loaded(ctx);
    if (err != HU_OK)
        return err;

    /* Build prompt: system + message */
    size_t prompt_cap = system_prompt_len + message_len + 64;
    char *prompt = (char *)alloc->alloc(alloc->ctx, prompt_cap);
    if (!prompt)
        return HU_ERR_OUT_OF_MEMORY;
    int pn = snprintf(prompt, prompt_cap, "%.*s\n\n%.*s", (int)system_prompt_len,
                      system_prompt ? system_prompt : "", (int)message_len, message ? message : "");
    size_t plen = (pn > 0 && (size_t)pn < prompt_cap) ? (size_t)pn : prompt_cap - 1;

    /* Tokenize */
    int32_t *input_ids = NULL;
    size_t input_count = 0;
    err = hu_bpe_tokenizer_encode(ctx->tokenizer, prompt, plen, &input_ids, &input_count);
    alloc->free(alloc->ctx, prompt, prompt_cap);
    if (err != HU_OK)
        return err;

    /* Generate tokens autoregressively */
    size_t max_gen = ctx->max_tokens > 0 ? ctx->max_tokens : 256;
    size_t vocab_size = 8192;
    char *result = (char *)alloc->alloc(alloc->ctx, max_gen * 4 + 1);
    if (!result) {
        alloc->free(alloc->ctx, input_ids, input_count * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t result_len = 0;

    hu_ml_tensor_t input_tensor = {0};
    input_tensor.data = input_ids;
    input_tensor.shape[0] = 1;
    input_tensor.shape[1] = input_count;
    input_tensor.ndim = 2;
    input_tensor.dtype = HU_ML_DTYPE_I32;
    input_tensor.size_bytes = input_count * sizeof(int32_t);

    hu_ml_tensor_t output = {0};
    err = ctx->model.vtable->forward(ctx->model.ctx, &input_tensor, &output);

    if (err != HU_OK || !output.data) {
        alloc->free(alloc->ctx, input_ids, input_count * sizeof(int32_t));
        if (output.data && output.size_bytes > 0)
            alloc->free(alloc->ctx, output.data, output.size_bytes);
        alloc->free(alloc->ctx, result, max_gen * 4 + 1);
        return err != HU_OK ? err : HU_ERR_IO;
    }

    {
        float *logits = (float *)output.data;
        size_t out_vocab = output.shape[output.ndim - 1];
        if (out_vocab == 0)
            out_vocab = vocab_size;

        /* Sample from the LAST position's logits (autoregressive) */
        float *last_logits = logits + (input_count - 1) * out_vocab;

        for (size_t step = 0; step < max_gen; step++) {
            int32_t next_token = sample_argmax(last_logits, out_vocab);
            if (next_token < 0)
                break;

            char *decoded = NULL;
            size_t dec_len = 0;
            hu_error_t dec_err =
                hu_bpe_tokenizer_decode(ctx->tokenizer, &next_token, 1, &decoded, &dec_len);
            if (dec_err != HU_OK || !decoded || dec_len == 0)
                break;
            if (result_len + dec_len >= max_gen * 4) {
                alloc->free(alloc->ctx, decoded, dec_len + 1);
                break;
            }
            memcpy(result + result_len, decoded, dec_len);
            result_len += dec_len;
            alloc->free(alloc->ctx, decoded, dec_len + 1);

            int32_t single = next_token;
            hu_ml_tensor_t step_in = {0};
            step_in.data = &single;
            step_in.shape[0] = 1;
            step_in.shape[1] = 1;
            step_in.ndim = 2;
            step_in.dtype = HU_ML_DTYPE_I32;
            step_in.size_bytes = sizeof(int32_t);

            if (output.data && output.size_bytes > 0)
                alloc->free(alloc->ctx, output.data, output.size_bytes);
            memset(&output, 0, sizeof(output));

            err = ctx->model.vtable->forward(ctx->model.ctx, &step_in, &output);
            if (err != HU_OK || !output.data)
                break;
            /* Single-token input: logits at position 0 */
            last_logits = (float *)output.data;
            out_vocab = output.shape[output.ndim - 1];
            if (out_vocab == 0)
                out_vocab = vocab_size;
        }
        if (output.data && output.size_bytes > 0)
            alloc->free(alloc->ctx, output.data, output.size_bytes);
    }

    alloc->free(alloc->ctx, input_ids, input_count * sizeof(int32_t));
    result[result_len] = '\0';
    *out = result;
    *out_len = result_len;
    return HU_OK;
#else
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    *out = NULL;
    *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

static hu_error_t huml_chat(void *ctx_ptr, hu_allocator_t *alloc, const hu_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    if (!ctx_ptr || !alloc || !request || !out)
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
    hu_error_t err = huml_chat_with_system(ctx_ptr, alloc, sys, sys_len, msg, msg_len, model,
                                           model_len, temperature, &content, &content_len);
    out->content = content;
    out->content_len = content_len;
    return err;
}

static bool huml_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *huml_get_name(void *ctx) {
    (void)ctx;
    return "huml";
}

static void huml_deinit(void *ctx_ptr, hu_allocator_t *alloc) {
    huml_ctx_t *ctx = (huml_ctx_t *)ctx_ptr;
    if (!ctx || !alloc)
        return;
#ifdef HU_ENABLE_ML
    if (ctx->active_lora) {
        huml_detach_active_lora(ctx);
        hu_lora_destroy(alloc, ctx->active_lora);
        ctx->active_lora = NULL;
    }
    if (ctx->loaded) {
        if (ctx->tokenizer)
            hu_bpe_tokenizer_deinit(ctx->tokenizer);
        if (ctx->optimizer.vtable)
            ctx->optimizer.vtable->deinit(ctx->optimizer.ctx, alloc);
        if (ctx->model.vtable)
            ctx->model.vtable->deinit(ctx->model.ctx, alloc);
    }
#endif
    alloc->free(alloc->ctx, ctx, sizeof(huml_ctx_t));
}

#ifdef HU_ENABLE_ML
/* W13 Phase 4 — attach the loaded adapter to the GPT's Q+V slots so
 * gpt_forward applies it on every token. Must be called only after
 * the model is loaded. Mirrors the training-time wiring in
 * `human ml lora-persona` (Q+V default). Adapter shape must match
 * HUML_GPT_N_EMBD on both axes; otherwise we leave the adapter in
 * the slot, mark unattached, and emit a one-shot stderr caveat so
 * the operator knows the adapter is loaded but inert. */
static void huml_try_attach_active_lora(huml_ctx_t *ctx) {
    if (!ctx || !ctx->loaded || !ctx->active_lora || ctx->active_lora_attached)
        return;
    size_t in_dim = 0, out_dim = 0, n_layers = 0;
    hu_lora_get_dims(ctx->active_lora, &in_dim, &out_dim, &n_layers);
    if (in_dim != HUML_GPT_N_EMBD || out_dim != HUML_GPT_N_EMBD ||
        n_layers != HUML_GPT_N_LAYER) {
        fprintf(stderr,
                "[huml] adapter '%s' shape (%zux%zu, %zu layers) does not "
                "match reference GPT (%dx%d, %d layers); not attaching to "
                "forward pass\n",
                ctx->active_adapter_id, in_dim, out_dim, n_layers,
                HUML_GPT_N_EMBD, HUML_GPT_N_EMBD, HUML_GPT_N_LAYER);
        return;
    }
    if (hu_gpt_attach_lora(&ctx->model, ctx->active_lora, NULL, ctx->active_lora,
                           NULL, NULL, NULL) == HU_OK)
        ctx->active_lora_attached = true;
}

static void huml_detach_active_lora(huml_ctx_t *ctx) {
    if (!ctx || !ctx->loaded || !ctx->active_lora_attached)
        return;
    (void)hu_gpt_attach_lora(&ctx->model, NULL, NULL, NULL, NULL, NULL, NULL);
    ctx->active_lora_attached = false;
}

/* W13 — adapter loading. Loads the adapter blob from disk via
 * hu_lora_load, stashes it under `adapter_id`, and (Phase 4) attaches
 * it to the live GPT's Q+V projections so the next forward pass is
 * actually biased by the adapter. If the model isn't loaded yet, we
 * remember the adapter and attach lazily when ensure_loaded() runs. */
static hu_error_t huml_load_adapter(void *ctx_ptr, const hu_lora_adapter_spec_t *spec,
                                    hu_lora_apply_mode_t mode) {
    huml_ctx_t *ctx = (huml_ctx_t *)ctx_ptr;
    if (!ctx || !spec)
        return HU_ERR_INVALID_ARGUMENT;
    /* S1.5 critic PE2: bytes-only spec is a "feature not supported here"
     * signal for init-08 federated LoRA capability detection — not a
     * caller bug. Returning HU_ERR_NOT_SUPPORTED lets init-08 probe
     * which providers can ingest pre-read weights without misreading
     * the result as an INVALID_ARGUMENT misuse. */
    if (spec->bytes && (!spec->path || spec->path_len == 0))
        return HU_ERR_NOT_SUPPORTED;
    /* Path-only is the only mode huml understands today. Anything else
     * (no source, or both source set — the latter caught by the helper
     * source-mutex, but we defend here for direct-vtable callers too). */
    if (!spec->path || spec->path_len == 0 || spec->bytes)
        return HU_ERR_INVALID_ARGUMENT;
    if (!spec->id || spec->id_len == 0 || !spec->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    /* S1.5 security review CRITICAL-1: reject unknown enum values. The
     * helper guards this for callers going through `hu_provider_load_adapter`
     * but direct-vtable callers (init-02 MoLoRA dispatcher) bypass that
     * fence. Self-defending impls cost one comparison. */
    if (mode != HU_LORA_APPLY_MODE_REPLACE && mode != HU_LORA_APPLY_MODE_STACK)
        return HU_ERR_INVALID_ARGUMENT;
    /* STACK is a MoLoRA primitive (init-02). huml is single-adapter only,
     * so we honor the contract by returning NOT_SUPPORTED rather than
     * silently downgrading — see lora.h docstring. */
    if (mode == HU_LORA_APPLY_MODE_STACK)
        return HU_ERR_NOT_SUPPORTED;

    /* S1.5 security review CRITICAL-1 / CWE-22: reject `..` traversal and
     * embedded NULs in `spec->path` before any fopen-equivalent. Mirrors
     * the mlx_qwen3 reference impl. Without this guard, a caller passing
     * `../../etc/shadow` would have it handed verbatim to `hu_lora_load`'s
     * fopen(), giving a path-probing oracle at minimum and arbitrary-file
     * read with magic-byte collision as the worst case. */
    if (spec->path_len > 0 && spec->path[spec->path_len - 1] == '\0')
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < spec->path_len; i++) {
        if (spec->path[i] == '\0')
            return HU_ERR_INVALID_ARGUMENT;
        if (i + 1 < spec->path_len && spec->path[i] == '.' && spec->path[i + 1] == '.')
            return HU_ERR_INVALID_ARGUMENT;
    }

    hu_allocator_t *alloc = spec->alloc;

    /* hu_lora_load takes a NUL-terminated path; copy so callers can pass
     * any slice. */
    char path_buf[512];
    if (spec->path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, spec->path, spec->path_len);
    path_buf[spec->path_len] = '\0';

    hu_lora_adapter_t *fresh = NULL;
    hu_error_t err = hu_lora_load(alloc, path_buf, &fresh);
    if (err != HU_OK)
        return err;

    /* Replace any incumbent only after the new one loads cleanly. */
    if (ctx->active_lora) {
        huml_detach_active_lora(ctx);
        hu_lora_destroy(alloc, ctx->active_lora);
        ctx->active_lora = NULL;
        ctx->active_adapter_id[0] = '\0';
    }

    ctx->active_lora = fresh;
    ctx->active_lora_attached = false;
    size_t copy_len = spec->id_len < sizeof(ctx->active_adapter_id) - 1
                          ? spec->id_len
                          : sizeof(ctx->active_adapter_id) - 1;
    memcpy(ctx->active_adapter_id, spec->id, copy_len);
    ctx->active_adapter_id[copy_len] = '\0';

    /* If the model is already loaded, attach now; otherwise the
     * lazy-load path in ensure_loaded() will attach after model load. */
    huml_try_attach_active_lora(ctx);
    return HU_OK;
}

static hu_error_t huml_unload_adapter(void *ctx_ptr, const char *adapter_id,
                                      size_t adapter_id_len) {
    huml_ctx_t *ctx = (huml_ctx_t *)ctx_ptr;
    if (!ctx || !adapter_id || adapter_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ctx->active_lora)
        return HU_OK; /* nothing to do; idempotent */
    /* Only drop when the id matches; this lets callers safely call
     * unload by id without worrying about race conditions where another
     * adapter has already replaced this one. */
    if (strncmp(ctx->active_adapter_id, adapter_id, adapter_id_len) != 0 ||
        ctx->active_adapter_id[adapter_id_len] != '\0')
        return HU_OK;
    if (!ctx->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    huml_detach_active_lora(ctx);
    hu_lora_destroy(ctx->alloc, ctx->active_lora);
    ctx->active_lora = NULL;
    ctx->active_adapter_id[0] = '\0';
    return HU_OK;
}

static const char *huml_active_adapter(void *ctx_ptr) {
    huml_ctx_t *ctx = (huml_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->active_lora || ctx->active_adapter_id[0] == '\0')
        return NULL;
    return ctx->active_adapter_id;
}
#endif /* HU_ENABLE_ML */

static const hu_provider_vtable_t huml_vtable = {
    .chat_with_system = huml_chat_with_system,
    .chat = huml_chat,
    .supports_native_tools = huml_supports_native_tools,
    .get_name = huml_get_name,
    .deinit = huml_deinit,
#ifdef HU_ENABLE_ML
    .load_adapter = huml_load_adapter,
    .unload_adapter = huml_unload_adapter,
    .active_adapter = huml_active_adapter,
#endif
};

hu_error_t hu_huml_provider_create(hu_allocator_t *alloc, const hu_huml_config_t *config,
                                   hu_provider_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    huml_ctx_t *ctx = (huml_ctx_t *)alloc->alloc(alloc->ctx, sizeof(huml_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    ctx->max_tokens = 256;

    if (config) {
        if (config->checkpoint_path && config->checkpoint_path_len > 0) {
            size_t clen = config->checkpoint_path_len < sizeof(ctx->checkpoint_path) - 1
                              ? config->checkpoint_path_len
                              : sizeof(ctx->checkpoint_path) - 1;
            memcpy(ctx->checkpoint_path, config->checkpoint_path, clen);
            ctx->checkpoint_path[clen] = '\0';
        }
        if (config->max_tokens > 0)
            ctx->max_tokens = config->max_tokens;
    }

    out->ctx = ctx;
    out->vtable = &huml_vtable;
    return HU_OK;
}
