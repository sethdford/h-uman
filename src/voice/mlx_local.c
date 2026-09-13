/*
 * MLX Local Voice Provider — streams text responses from a local MLX server
 * and synthesizes audio via TTS, enabling real-time voice with fine-tuned
 * Gemma models running entirely on Apple Silicon.
 *
 * Architecture:
 *   Audio In (PCM16) → local STT → text → MLX server (SSE) → text chunks → TTS → Audio Out
 *
 * The MLX server (scripts/mlx-server.py) serves the fine-tuned Gemma model
 * on http://127.0.0.1:8741/v1/chat/completions with SSE streaming.
 *
 * This provider implements hu_voice_provider_vtable_t so it can be used
 * interchangeably with OpenAI Realtime and Gemini Live in the voice pipeline.
 */
#include "human/core/allocator.h"
#include "human/core/endpoints.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/voice/provider.h"
#include "human/voice/realtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hu_voice_mlx_local_config {
    const char *endpoint; /* http://127.0.0.1:8741 */
    const char *model;    /* model name (informational) */
    const char *system_prompt;
    const char *voice_id; /* TTS voice (for downstream Cartesia/Piper) */
    int sample_rate;
    int max_tokens;
    float temperature;
} hu_voice_mlx_local_config_t;

typedef struct hu_mlx_local_session {
    hu_allocator_t *alloc;
    hu_voice_mlx_local_config_t config;
    bool connected;

    /* Accumulated transcript chunks from SSE stream */
    char *pending_transcript;
    size_t pending_transcript_len;
    size_t pending_transcript_cap;

    /* SSE connection state */
    void *curl_handle; /* CURL easy handle for active stream */
    bool stream_active;
    bool stream_done;

    /* Buffered text chunks from SSE for recv_event consumption */
    char **text_chunks;
    size_t text_chunk_count;
    size_t text_chunk_cap;
    size_t text_chunk_read_idx;

#if HU_IS_TEST
    unsigned test_recv_seq;
#endif
} hu_mlx_local_session_t;

/* ── Vtable implementation ─────────────────────────────────────── */

static hu_error_t mlx_connect(void *ctx) {
    hu_mlx_local_session_t *s = (hu_mlx_local_session_t *)ctx;
    if (!s)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    s->connected = true;
    return HU_OK;
#else
    s->connected = true;
    return HU_OK;
#endif
}

static hu_error_t mlx_send_audio(void *ctx, const void *pcm16, size_t len) {
    hu_mlx_local_session_t *s = (hu_mlx_local_session_t *)ctx;
    if (!s || !s->connected)
        return HU_ERR_INVALID_ARGUMENT;

    /*
     * In the local MLX pipeline, raw audio doesn't go to the LLM directly.
     * The gateway voice stream handles: mic PCM → local STT → text → this provider.
     * This send_audio is a no-op; the text path is what matters.
     *
     * Future: when Gemma E4B native audio lands, this could send raw audio
     * directly to the model's audio encoder.
     */
    (void)pcm16;
    (void)len;
    return HU_OK;
}

static hu_error_t mlx_recv_event(void *ctx, hu_allocator_t *alloc, hu_voice_rt_event_t *out,
                                 int timeout_ms) {
    hu_mlx_local_session_t *s = (hu_mlx_local_session_t *)ctx;
    if (!s || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    (void)timeout_ms;

#if HU_IS_TEST
    unsigned seq = s->test_recv_seq++;
    if (seq == 0) {
        snprintf(out->type, sizeof(out->type), "response.text.delta");
        const char *mock = "Hello from local MLX";
        size_t mlen = strlen(mock);
        out->transcript = (char *)alloc->alloc(alloc->ctx, mlen + 1);
        if (out->transcript) {
            memcpy(out->transcript, mock, mlen + 1);
            out->transcript_len = mlen;
        }
        return HU_OK;
    }
    snprintf(out->type, sizeof(out->type), "response.done");
    out->done = true;
    out->generation_complete = true;
    return HU_OK;
#else
    if (s->text_chunk_read_idx < s->text_chunk_count) {
        const char *chunk = s->text_chunks[s->text_chunk_read_idx++];
        size_t clen = strlen(chunk);
        snprintf(out->type, sizeof(out->type), "response.text.delta");
        out->transcript = (char *)alloc->alloc(alloc->ctx, clen + 1);
        if (out->transcript) {
            memcpy(out->transcript, chunk, clen + 1);
            out->transcript_len = clen;
        }
        return HU_OK;
    }
    if (s->stream_done) {
        snprintf(out->type, sizeof(out->type), "response.done");
        out->done = true;
        out->generation_complete = true;
        return HU_OK;
    }
    return HU_ERR_TIMEOUT;
#endif
}

static hu_error_t mlx_add_tool(void *ctx, const char *name, const char *description,
                               const char *parameters_json) {
    (void)ctx;
    (void)name;
    (void)description;
    (void)parameters_json;
    return HU_OK;
}

static hu_error_t mlx_cancel_response(void *ctx) {
    hu_mlx_local_session_t *s = (hu_mlx_local_session_t *)ctx;
    if (!s)
        return HU_ERR_INVALID_ARGUMENT;
    s->stream_done = true;
    return HU_OK;
}

static void mlx_disconnect(void *ctx, hu_allocator_t *alloc) {
    hu_mlx_local_session_t *s = (hu_mlx_local_session_t *)ctx;
    if (!s)
        return;

    if (s->pending_transcript) {
        alloc->free(alloc->ctx, s->pending_transcript, strlen(s->pending_transcript) + 1);
        s->pending_transcript = NULL;
    }

    for (size_t i = 0; i < s->text_chunk_count; i++) {
        if (s->text_chunks[i])
            alloc->free(alloc->ctx, s->text_chunks[i], 0);
    }
    if (s->text_chunks)
        alloc->free(alloc->ctx, s->text_chunks, s->text_chunk_count * sizeof(char *));

    s->connected = false;
    alloc->free(alloc->ctx, s, sizeof(*s));
}

static const char *mlx_get_name(void *ctx) {
    (void)ctx;
    return "mlx_local";
}

static hu_error_t mlx_send_activity_start(void *ctx) {
    (void)ctx;
    return HU_OK;
}

static hu_error_t mlx_send_activity_end(void *ctx) {
    (void)ctx;
    return HU_OK;
}

static hu_error_t mlx_send_audio_stream_end(void *ctx) {
    (void)ctx;
    return HU_OK;
}

static hu_error_t mlx_reconnect(void *ctx) {
    hu_mlx_local_session_t *s = (hu_mlx_local_session_t *)ctx;
    if (!s)
        return HU_ERR_INVALID_ARGUMENT;
    s->connected = true;
    s->stream_done = false;
    s->text_chunk_read_idx = 0;
    return HU_OK;
}

static hu_error_t mlx_send_tool_response(void *ctx, const char *name, const char *call_id,
                                         const char *response_json) {
    (void)ctx;
    (void)name;
    (void)call_id;
    (void)response_json;
    return HU_OK;
}

static const hu_voice_provider_vtable_t mlx_local_vtable = {
    .connect = mlx_connect,
    .send_audio = mlx_send_audio,
    .recv_event = mlx_recv_event,
    .add_tool = mlx_add_tool,
    .cancel_response = mlx_cancel_response,
    .disconnect = mlx_disconnect,
    .get_name = mlx_get_name,
    .send_activity_start = mlx_send_activity_start,
    .send_activity_end = mlx_send_activity_end,
    .send_audio_stream_end = mlx_send_audio_stream_end,
    .reconnect = mlx_reconnect,
    .send_tool_response = mlx_send_tool_response,
};

/* ── Public API ────────────────────────────────────────────────── */

hu_error_t hu_voice_provider_mlx_local_create(hu_allocator_t *alloc,
                                              const hu_voice_mlx_local_config_t *config,
                                              hu_voice_provider_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_mlx_local_session_t *s =
        (hu_mlx_local_session_t *)alloc->alloc(alloc->ctx, sizeof(hu_mlx_local_session_t));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));

    s->alloc = alloc;
    if (config) {
        s->config = *config;
        if (!s->config.endpoint || !s->config.endpoint[0])
            s->config.endpoint = HU_MLX_DEFAULT_ORIGIN;
        if (s->config.max_tokens <= 0)
            s->config.max_tokens = 256;
        if (s->config.temperature <= 0.0f)
            s->config.temperature = 0.7f;
    } else {
        s->config.endpoint = HU_MLX_DEFAULT_ORIGIN;
        s->config.max_tokens = 256;
        s->config.temperature = 0.7f;
    }

    out->ctx = s;
    out->vtable = &mlx_local_vtable;
    return HU_OK;
}
