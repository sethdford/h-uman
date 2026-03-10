#ifndef HU_PROVIDER_H
#define HU_PROVIDER_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
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
} hu_chat_response_t;

typedef struct hu_stream_chunk {
    const char *delta;
    size_t delta_len;
    bool is_final;
    uint32_t token_count;
} hu_stream_chunk_t;

typedef struct hu_stream_chat_result {
    const char *content; /* optional, NULL if none */
    size_t content_len;
    hu_token_usage_t usage;
    const char *model;
    size_t model_len;
} hu_stream_chat_result_t;

typedef void (*hu_stream_callback_t)(void *ctx, const hu_stream_chunk_t *chunk);

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
} hu_provider_vtable_t;

/* Free allocations in a chat response (content, model, tool_calls and their strings). */
void hu_chat_response_free(hu_allocator_t *alloc, hu_chat_response_t *resp);

const char *hu_compatible_provider_url(const char *name);

#endif /* HU_PROVIDER_H */
