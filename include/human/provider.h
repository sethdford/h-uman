#ifndef HU_PROVIDER_H
#define HU_PROVIDER_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include "human/lora.h" /* hu_lora_adapter_spec_t, hu_lora_apply_mode_t (S1.5 (a)) */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Provider types — LLM chat, tool calls, streaming
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_image_detail {
    HU_IMAGE_DETAIL_AUTO,
    HU_IMAGE_DETAIL_LOW,
    HU_IMAGE_DETAIL_HIGH,
} hu_image_detail_t;

typedef enum hu_role {
    HU_ROLE_SYSTEM,
    HU_ROLE_USER,
    HU_ROLE_ASSISTANT,
    HU_ROLE_TOOL,
} hu_role_t;

typedef struct hu_content_part_image_url {
    const char *url;
    size_t url_len;
    hu_image_detail_t detail;
} hu_content_part_image_url_t;

typedef struct hu_content_part_image_base64 {
    const char *data;
    size_t data_len;
    const char *media_type;
    size_t media_type_len;
} hu_content_part_image_base64_t;

typedef struct hu_content_part_audio_base64 {
    const char *data;
    size_t data_len;
    const char *media_type; /* e.g. "audio/wav", "audio/mp3" */
    size_t media_type_len;
} hu_content_part_audio_base64_t;

typedef struct hu_content_part_video_url {
    const char *url;
    size_t url_len;
    const char *media_type; /* e.g. "video/mp4" */
    size_t media_type_len;
} hu_content_part_video_url_t;

typedef enum hu_content_part_tag {
    HU_CONTENT_PART_TEXT,
    HU_CONTENT_PART_IMAGE_URL,
    HU_CONTENT_PART_IMAGE_BASE64,
    HU_CONTENT_PART_AUDIO_BASE64,
    HU_CONTENT_PART_VIDEO_URL,
} hu_content_part_tag_t;

typedef struct hu_content_part {
    hu_content_part_tag_t tag;
    union {
        struct {
            const char *ptr;
            size_t len;
        } text;
        hu_content_part_image_url_t image_url;
        hu_content_part_image_base64_t image_base64;
        hu_content_part_audio_base64_t audio_base64;
        hu_content_part_video_url_t video_url;
    } data;
} hu_content_part_t;

typedef struct hu_tool_call hu_tool_call_t;

typedef struct hu_chat_message {
    hu_role_t role;
    const char *content;
    size_t content_len;
    const char *name;                       /* optional, for tool results */
    size_t name_len;                        /* 0 if name is NULL */
    const char *tool_call_id;               /* optional */
    size_t tool_call_id_len;                /* 0 if tool_call_id is NULL */
    const hu_content_part_t *content_parts; /* optional, NULL if not used */
    size_t content_parts_count;             /* 0 if content_parts is NULL */
    const hu_tool_call_t *tool_calls;       /* optional, for assistant messages */
    size_t tool_calls_count;                /* 0 if tool_calls is NULL */
} hu_chat_message_t;

typedef struct hu_tool_call {
    const char *id;
    size_t id_len;
    const char *name;
    size_t name_len;
    const char *arguments;
    size_t arguments_len;
} hu_tool_call_t;

typedef struct hu_token_usage {
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
} hu_token_usage_t;

typedef struct hu_chat_response {
    const char *content; /* optional, NULL if none */
    size_t content_len;  /* 0 if content is NULL */
    const hu_tool_call_t *tool_calls;
    size_t tool_calls_count;
    hu_token_usage_t usage;
    const char *model;
    size_t model_len;
    const char *reasoning_content; /* optional, NULL if none */
    size_t reasoning_content_len;  /* 0 if reasoning_content is NULL */
    /* Mean logprob over completion tokens when provider returned logprobs (e.g. OpenAI). */
    bool logprob_mean_valid;
    float logprob_mean;
} hu_chat_response_t;

typedef enum hu_stream_chunk_type {
    HU_STREAM_CONTENT = 0, /* text delta (default / backward-compatible) */
    HU_STREAM_TOOL_START,  /* tool call beginning: tool_name + tool_call_id set */
    HU_STREAM_TOOL_DELTA,  /* tool call arguments delta in 'delta' */
    HU_STREAM_TOOL_DONE,   /* tool call complete for this tool_index */
    HU_STREAM_THINKING,    /* reasoning/thinking content delta */
} hu_stream_chunk_type_t;

typedef struct hu_stream_chunk {
    hu_stream_chunk_type_t type; /* HU_STREAM_CONTENT if unset (zero-init) */
    const char *delta;
    size_t delta_len;
    bool is_final;
    uint32_t token_count;
    /* Tool-specific fields (only meaningful for HU_STREAM_TOOL_*) */
    const char *tool_name;
    size_t tool_name_len;
    const char *tool_call_id;
    size_t tool_call_id_len;
    int tool_index; /* distinguishes parallel tool calls within one turn */
} hu_stream_chunk_t;

typedef struct hu_stream_chat_result {
    const char *content; /* optional, NULL if none */
    size_t content_len;
    hu_token_usage_t usage;
    const char *model;
    size_t model_len;
    /* Accumulated tool calls from streaming (NULL if none) */
    hu_tool_call_t *tool_calls;
    size_t tool_calls_count;
    const char *reasoning_content; /* accumulated thinking deltas, NULL if none */
    size_t reasoning_content_len;
} hu_stream_chat_result_t;

/* Free allocations in a stream chat result (content, model, tool_calls). */
void hu_stream_chat_result_free(hu_allocator_t *alloc, hu_stream_chat_result_t *result);

/* Returns true to continue streaming, false to signal the provider to stop. */
typedef bool (*hu_stream_callback_t)(void *ctx, const hu_stream_chunk_t *chunk);

typedef struct hu_tool_spec {
    const char *name;
    size_t name_len;
    const char *description;
    size_t description_len;
    const char *parameters_json;
    size_t parameters_json_len; /* default "{}" if 0 */
} hu_tool_spec_t;

typedef struct hu_chat_request {
    const hu_chat_message_t *messages;
    size_t messages_count;
    const char *model;
    size_t model_len;
    double temperature;           /* default 0.7 */
    uint32_t max_tokens;          /* 0 = provider default */
    const hu_tool_spec_t *tools;  /* optional, NULL if none */
    size_t tools_count;           /* 0 if tools is NULL */
    uint64_t timeout_secs;        /* 0 = no limit */
    const char *reasoning_effort; /* optional, NULL = don't send */
    size_t reasoning_effort_len;
    const char *response_format; /* optional: "json_object", "json_schema", NULL = default */
    size_t response_format_len;
    double budget_remaining_usd;      /* 0.0 = unlimited; router may downgrade model when low */
    int thinking_budget;              /* 0 = no thinking; >0 = token budget for model reasoning */
    bool include_completion_logprobs; /* OpenAI-compatible: logprobs + top_logprobs in request */
    const char *prompt_cache_id; /* provider-level cache ID for system prompt dedup; NULL = none */
    size_t prompt_cache_id_len;
} hu_chat_request_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Provider vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_provider_vtable;

typedef struct hu_provider {
    void *ctx;
    const struct hu_provider_vtable *vtable;
} hu_provider_t;

typedef struct hu_provider_vtable {
    /* Required */
    hu_error_t (*chat_with_system)(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                   size_t system_prompt_len, const char *message,
                                   size_t message_len, const char *model, size_t model_len,
                                   double temperature, char **out, size_t *out_len);

    hu_error_t (*chat)(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                       const char *model, size_t model_len, double temperature,
                       hu_chat_response_t *out);

    bool (*supports_native_tools)(void *ctx);
    const char *(*get_name)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);

    /* Optional — may be NULL */
    void (*warmup)(void *ctx);
    hu_error_t (*chat_with_tools)(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                  hu_chat_response_t *out);
    bool (*supports_streaming)(void *ctx);
    bool (*supports_vision)(void *ctx);
    bool (*supports_vision_for_model)(void *ctx, const char *model, size_t model_len);
    hu_error_t (*stream_chat)(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              hu_stream_callback_t callback, void *callback_ctx,
                              hu_stream_chat_result_t *out);

    /* W13 / SOTA-2026 init-04 — On-device personalization adapter loading.
     * Optional triple. A provider that supports LoRA/PEFT-style adapter
     * swapping at chat time implements all three. Providers that don't
     * (cloud APIs) leave the pointers NULL and callers should fall back
     * to the no-adapter path.
     *
     *   load_adapter:    Bind the adapter described by `spec` and apply
     *                    according to `mode` (REPLACE — default, swap any
     *                    incumbent; STACK — additive composition for
     *                    MoLoRA, init-02). `spec` is short-lived; the
     *                    provider MUST copy any fields it wants to keep.
     *                    Providers that do not support STACK MUST return
     *                    HU_ERR_NOT_SUPPORTED for that mode rather than
     *                    silently downgrading — the MoLoRA dispatcher
     *                    needs accurate state.
     *   unload_adapter:  Drop the adapter; safe-NULL-arg if not currently
     *                    loaded. After unload the next chat call falls
     *                    back to the base model.
     *   active_adapter:  Return the id of the currently active adapter,
     *                    or NULL when none is loaded. Owned by the
     *                    provider (caller must NOT free).
     *
     * The contract is intentionally narrow so cloud providers can leave
     * the pointers NULL with no churn. The local huml provider is the
     * first reference implementation.
     *
     * S1.5 (a): signature was widened from (alloc, path, path_len, id,
     * id_len) to (spec, mode). The path-based fields moved into
     * `hu_lora_adapter_spec_t`; the mode is new and supports the locked
     * convention from `docs/plans/2026-05-11-sota-2026-massive-team-program.md`
     * §"hu_provider_t.load_adapter surface". Cloud-safety contract is
     * unchanged: providers that leave this NULL still fall through to
     * HU_ERR_NOT_SUPPORTED via the helper. */
    hu_error_t (*load_adapter)(void *ctx, const hu_lora_adapter_spec_t *spec,
                               hu_lora_apply_mode_t mode);
    hu_error_t (*unload_adapter)(void *ctx, const char *adapter_id, size_t adapter_id_len);
    const char *(*active_adapter)(void *ctx);

    /* SOTA-2026 init-01 — activation steering. Optional. Cloud
     * providers leave this NULL; on-device providers (llamacpp,
     * embedded, future mlx_qwen3) implement it.
     *
     * `vec` is a small abstract trait-coefficient vector of `dim`
     * floats produced by `hu_persona_steering_vector(...)`. The
     * provider is responsible for mapping each coefficient to
     * its model's residual stream (typically via a precomputed
     * SAE-decoder projection or CAA-style difference vector at
     * a configured layer index).
     *
     * Calling with vec == NULL and dim == 0 disables steering
     * (resets to base). Returning HU_ERR_NOT_SUPPORTED is legal
     * at any time — callers MUST fall through to the prompt-side
     * directive path.
     *
     * Contract:
     *   - `vec` is owned by the caller; the provider must copy
     *     any state it wants to retain across chat() calls.
     *   - `dim` is bounded by HU_STEERING_VEC_MAX_DIM. Providers
     *     MUST reject larger dims with HU_ERR_INVALID_ARGUMENT.
     *   - Steering applies to subsequent chat()/stream_chat()
     *     calls on the same provider context until
     *     `apply_steering(NULL, 0)` resets it or a new vector
     *     overwrites. */
    hu_error_t (*apply_steering)(void *ctx, const float *vec, size_t dim);
} hu_provider_vtable_t;

/* SOTA-2026 init-01 — hard cap on the abstract steering vector
 * dimension. The persona-layer header (`human/persona/steering.h`)
 * defines the canonical fixed dim (HU_STEERING_VEC_DIM = 32); this
 * boundary is what providers and the dispatch helper reject above.
 * Kept here so callers don't have to pull the persona header just
 * to validate a dim. */
#ifndef HU_STEERING_VEC_MAX_DIM
#define HU_STEERING_VEC_MAX_DIM 64
#endif

/* Optional adapter-loading helpers. Each returns HU_ERR_NOT_SUPPORTED when
 * the provider's vtable leaves the corresponding method NULL.
 *
 * S1.5 (a): `hu_provider_load_adapter` was widened to take a
 * `hu_lora_adapter_spec_t *` + `hu_lora_apply_mode_t` (matches the new
 * vtable surface). Cloud-safety contract: NULL `vtable->load_adapter`
 * → HU_ERR_NOT_SUPPORTED, pinned by `tests/test_provider_all.c::
 * test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`. */
hu_error_t hu_provider_load_adapter(hu_provider_t *provider,
                                    const hu_lora_adapter_spec_t *spec,
                                    hu_lora_apply_mode_t mode);
hu_error_t hu_provider_unload_adapter(hu_provider_t *provider, const char *adapter_id,
                                      size_t adapter_id_len);
/* Returns the active adapter id (provider-owned, caller must not free), or
 * NULL when none is loaded or the provider doesn't support adapters. */
const char *hu_provider_active_adapter(const hu_provider_t *provider);

/* SOTA-2026 init-01 — activation steering dispatch helper.
 *
 * Mirrors `hu_provider_load_adapter`'s safe-no-op pattern. Returns
 * HU_ERR_NOT_SUPPORTED when the provider's vtable leaves
 * `apply_steering` NULL (the cloud-provider common case); callers
 * (agent_turn.c) treat that as a signal to inject the prompt-side
 * directive instead. Returns HU_OK on success; residual-stream
 * steering took the load.
 *
 * Validates argument bounds (`dim <= HU_STEERING_VEC_MAX_DIM`,
 * non-NULL vec when dim > 0) before dispatch so providers never
 * have to repeat those checks. Calling with `vec == NULL && dim == 0`
 * is the reset form and is forwarded to the provider verbatim. */
hu_error_t hu_provider_apply_steering(hu_provider_t *provider, const float *vec, size_t dim);

/* Free allocations in a chat response (content, model, tool_calls and their strings). */
void hu_chat_response_free(hu_allocator_t *alloc, hu_chat_response_t *resp);

const char *hu_compatible_provider_url(const char *name);

#endif /* HU_PROVIDER_H */
