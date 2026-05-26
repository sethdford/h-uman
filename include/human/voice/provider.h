#ifndef HU_VOICE_PROVIDER_H
#define HU_VOICE_PROVIDER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/voice/realtime.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Voice Provider abstraction — vtable interface for swappable voice backends.
 * Currently wraps OpenAI Realtime; designed for Twilio, Google, Deepgram, etc.
 */

typedef struct hu_voice_provider_vtable {
    hu_error_t (*connect)(void *ctx);
    hu_error_t (*send_audio)(void *ctx, const void *pcm16, size_t len);
    hu_error_t (*recv_event)(void *ctx, hu_allocator_t *alloc, hu_voice_rt_event_t *out,
                             int timeout_ms);
    hu_error_t (*add_tool)(void *ctx, const char *name, const char *description,
                           const char *parameters_json);
    hu_error_t (*cancel_response)(void *ctx);
    void (*disconnect)(void *ctx, hu_allocator_t *alloc);
    const char *(*get_name)(void *ctx);
    hu_error_t (*send_activity_start)(void *ctx);
    hu_error_t (*send_activity_end)(void *ctx);
    hu_error_t (*send_audio_stream_end)(void *ctx);
    hu_error_t (*reconnect)(void *ctx);
    hu_error_t (*send_tool_response)(void *ctx, const char *name, const char *call_id,
                                     const char *response_json);
} hu_voice_provider_vtable_t;

typedef struct hu_voice_provider {
    void *ctx;
    const hu_voice_provider_vtable_t *vtable;
} hu_voice_provider_t;

/* Create an OpenAI Realtime voice provider (wraps hu_voice_rt_session_t). */
hu_error_t hu_voice_provider_openai_create(hu_allocator_t *alloc,
                                           const hu_voice_rt_config_t *config,
                                           hu_voice_provider_t *out);

struct hu_gemini_live_config;
hu_error_t hu_voice_provider_gemini_live_create(hu_allocator_t *alloc,
                                                const struct hu_gemini_live_config *config,
                                                hu_voice_provider_t *out);

typedef struct hu_voice_provider_extras {
    const char *system_instruction;
    const char *tools_json;
    const char *voice_id;
    const char *model_id;
    const char *api_key;
    const char *vertex_region;
    const char *vertex_project;
    const char *vertex_access_token;
    int sample_rate;
} hu_voice_provider_extras_t;

struct hu_config;
hu_error_t hu_voice_provider_create_from_config(hu_allocator_t *alloc,
                                                const struct hu_config *config, const char *mode,
                                                const hu_voice_provider_extras_t *extras,
                                                hu_voice_provider_t *out);

hu_error_t hu_voice_provider_create_from_extras(hu_allocator_t *alloc, const char *mode,
                                                const hu_voice_provider_extras_t *extras,
                                                hu_voice_provider_t *out);

/* MLX local voice provider — see human/voice/mlx_local.h for config struct.
 * Renamed 2026-05-26 from hu_mlx_local_config to hu_voice_mlx_local_config to
 * avoid collision with the M3 B4 streaming-gate hu_mlx_local_config_t in
 * include/human/config_types.h (different struct, same name). */
struct hu_voice_mlx_local_config;
hu_error_t hu_voice_provider_mlx_local_create(hu_allocator_t *alloc,
                                              const struct hu_voice_mlx_local_config *config,
                                              hu_voice_provider_t *out);

#endif
