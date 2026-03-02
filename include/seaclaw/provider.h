#ifndef SC_PROVIDER_H
#define SC_PROVIDER_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Provider types — LLM chat, tool calls, streaming
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_image_detail {
    SC_IMAGE_DETAIL_AUTO,
    SC_IMAGE_DETAIL_LOW,
    SC_IMAGE_DETAIL_HIGH,
} sc_image_detail_t;

typedef enum sc_role {
    SC_ROLE_SYSTEM,
    SC_ROLE_USER,
    SC_ROLE_ASSISTANT,
    SC_ROLE_TOOL,
} sc_role_t;

typedef struct sc_content_part_image_url {
    const char *url;
    size_t url_len;
    sc_image_detail_t detail;
} sc_content_part_image_url_t;

typedef struct sc_content_part_image_base64 {
    const char *data;
    size_t data_len;
    const char *media_type;
    size_t media_type_len;
} sc_content_part_image_base64_t;

typedef enum sc_content_part_tag {
    SC_CONTENT_PART_TEXT,
    SC_CONTENT_PART_IMAGE_URL,
    SC_CONTENT_PART_IMAGE_BASE64,
} sc_content_part_tag_t;

typedef struct sc_content_part {
    sc_content_part_tag_t tag;
    union {
        struct { const char *ptr; size_t len; } text;
        sc_content_part_image_url_t image_url;
        sc_content_part_image_base64_t image_base64;
    } data;
} sc_content_part_t;

typedef struct sc_tool_call sc_tool_call_t;

typedef struct sc_chat_message {
    sc_role_t role;
    const char *content;
    size_t content_len;
    const char *name;      /* optional, for tool results */
    size_t name_len;       /* 0 if name is NULL */
    const char *tool_call_id; /* optional */
    size_t tool_call_id_len; /* 0 if tool_call_id is NULL */
    const sc_content_part_t *content_parts; /* optional, NULL if not used */
    size_t content_parts_count;              /* 0 if content_parts is NULL */
    const sc_tool_call_t *tool_calls; /* optional, for assistant messages */
    size_t tool_calls_count;          /* 0 if tool_calls is NULL */
} sc_chat_message_t;

typedef struct sc_tool_call {
    const char *id;
    size_t id_len;
    const char *name;
    size_t name_len;
    const char *arguments;
    size_t arguments_len;
} sc_tool_call_t;

typedef struct sc_token_usage {
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
} sc_token_usage_t;

typedef struct sc_chat_response {
    const char *content;       /* optional, NULL if none */
    size_t content_len;        /* 0 if content is NULL */
    const sc_tool_call_t *tool_calls;
    size_t tool_calls_count;
    sc_token_usage_t usage;
    const char *model;
    size_t model_len;
    const char *reasoning_content; /* optional, NULL if none */
    size_t reasoning_content_len; /* 0 if reasoning_content is NULL */
} sc_chat_response_t;

typedef struct sc_stream_chunk {
    const char *delta;
    size_t delta_len;
    bool is_final;
    uint32_t token_count;
} sc_stream_chunk_t;

typedef struct sc_stream_chat_result {
    const char *content;       /* optional, NULL if none */
    size_t content_len;
    sc_token_usage_t usage;
    const char *model;
    size_t model_len;
} sc_stream_chat_result_t;

typedef void (*sc_stream_callback_t)(void *ctx, const sc_stream_chunk_t *chunk);

typedef struct sc_tool_spec {
    const char *name;
    size_t name_len;
    const char *description;
    size_t description_len;
    const char *parameters_json;
    size_t parameters_json_len; /* default "{}" if 0 */
} sc_tool_spec_t;

typedef struct sc_chat_request {
    const sc_chat_message_t *messages;
    size_t messages_count;
    const char *model;
    size_t model_len;
    double temperature;       /* default 0.7 */
    uint32_t max_tokens;      /* 0 = provider default */
    const sc_tool_spec_t *tools; /* optional, NULL if none */
    size_t tools_count;       /* 0 if tools is NULL */
    uint64_t timeout_secs;    /* 0 = no limit */
    const char *reasoning_effort; /* optional, NULL = don't send */
    size_t reasoning_effort_len;
} sc_chat_request_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Provider vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_provider_vtable;

typedef struct sc_provider {
    void *ctx;
    const struct sc_provider_vtable *vtable;
} sc_provider_t;

typedef struct sc_provider_vtable {
    /* Required */
    sc_error_t (*chat_with_system)(void *ctx, sc_allocator_t *alloc,
        const char *system_prompt, size_t system_prompt_len,
        const char *message, size_t message_len,
        const char *model, size_t model_len,
        double temperature,
        char **out, size_t *out_len);

    sc_error_t (*chat)(void *ctx, sc_allocator_t *alloc,
        const sc_chat_request_t *request,
        const char *model, size_t model_len,
        double temperature,
        sc_chat_response_t *out);

    bool (*supports_native_tools)(void *ctx);
    const char *(*get_name)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);

    /* Optional — may be NULL */
    void (*warmup)(void *ctx);
    sc_error_t (*chat_with_tools)(void *ctx, sc_allocator_t *alloc,
        const sc_chat_request_t *req,
        sc_chat_response_t *out);
    bool (*supports_streaming)(void *ctx);
    bool (*supports_vision)(void *ctx);
    bool (*supports_vision_for_model)(void *ctx, const char *model, size_t model_len);
    sc_error_t (*stream_chat)(void *ctx, sc_allocator_t *alloc,
        const sc_chat_request_t *request,
        const char *model, size_t model_len,
        double temperature,
        sc_stream_callback_t callback, void *callback_ctx,
        sc_stream_chat_result_t *out);
} sc_provider_vtable_t;

/* Free allocations in a chat response (content, model, tool_calls and their strings). */
void sc_chat_response_free(sc_allocator_t *alloc, sc_chat_response_t *resp);

const char *sc_compatible_provider_url(const char *name);

#endif /* SC_PROVIDER_H */
