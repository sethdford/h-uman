/*
 * Cartesia TTS WebSocket streaming client (pcm_f32le @ 24kHz).
 */
#include "human/tts/cartesia_stream.h"
#include "human/core/json.h"
#include "human/multimodal.h"
#include "human/websocket/websocket.h"
#include <stdio.h>
#include <string.h>

struct hu_cartesia_stream {
    hu_allocator_t *alloc;
    hu_ws_client_t *ws;
    char model_id[128];
    char voice_id[128];
    hu_cartesia_voice_controls_t voice_controls;
    bool has_voice_controls;
#if HU_IS_TEST
    unsigned mock_send_count;
    unsigned mock_pending_chunks; /* audio queued by sends, not yet received */
    bool mock_flushed;            /* context closed; "done" follows the queued audio */
#endif
};

void hu_cartesia_stream_set_voice_controls(hu_cartesia_stream_t *s,
                                           const hu_cartesia_voice_controls_t *controls) {
    if (!s)
        return;
    if (controls) {
        s->voice_controls = *controls;
        s->has_voice_controls = true;
    } else {
        memset(&s->voice_controls, 0, sizeof(s->voice_controls));
        s->has_voice_controls = false;
    }
}

#if HU_IS_TEST

hu_error_t hu_cartesia_stream_open(hu_allocator_t *alloc, const char *api_key, const char *voice_id,
                                   const char *model_id, hu_cartesia_stream_t **out) {
    if (!alloc || !api_key || !out)
        return HU_ERR_INVALID_ARGUMENT;
    (void)api_key;
    hu_cartesia_stream_t *s =
        (hu_cartesia_stream_t *)alloc->alloc(alloc->ctx, sizeof(hu_cartesia_stream_t));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->ws = (hu_ws_client_t *)1; /* non-NULL sentinel */
    if (voice_id && voice_id[0])
        (void)snprintf(s->voice_id, sizeof(s->voice_id), "%s", voice_id);
    if (model_id && model_id[0])
        (void)snprintf(s->model_id, sizeof(s->model_id), "%s", model_id);
    *out = s;
    return HU_OK;
}

void hu_cartesia_stream_close(hu_cartesia_stream_t *s, hu_allocator_t *alloc) {
    if (!s || !alloc)
        return;
    alloc->free(alloc->ctx, s, sizeof(*s));
}

hu_error_t hu_cartesia_stream_send_generation(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                              const char *context_id, const char *transcript,
                                              bool is_continue) {
    (void)context_id;
    (void)transcript;
    (void)is_continue;
    if (!s || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    s->mock_send_count++;
    s->mock_pending_chunks++;
    s->mock_flushed = false;
    return HU_OK;
}

hu_error_t hu_cartesia_stream_flush_context(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                            const char *context_id) {
    (void)context_id;
    if (!s || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    s->mock_send_count++;
    s->mock_flushed = true;
    return HU_OK;
}

hu_error_t hu_cartesia_stream_cancel_context(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                             const char *context_id) {
    (void)context_id;
    if (!s || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    s->mock_pending_chunks = 0;
    s->mock_flushed = false;
    return HU_OK;
}

/* Mirrors the service: each send yields one audio chunk; "done" comes only after the
 * context is flushed; with nothing to deliver on an open context the wait times out. */
hu_error_t hu_cartesia_stream_recv_next_wait(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                             int timeout_ms, void **pcm_out, size_t *pcm_len,
                                             bool *recv_done) {
    (void)timeout_ms;
    if (!s || !alloc || !pcm_out || !pcm_len || !recv_done)
        return HU_ERR_INVALID_ARGUMENT;
    *pcm_out = NULL;
    *pcm_len = 0;
    *recv_done = false;

    if (s->mock_pending_chunks > 0) {
        s->mock_pending_chunks--;
        size_t nsamp = 64;
        size_t nbytes = nsamp * sizeof(float);
        float *pcm = (float *)alloc->alloc(alloc->ctx, nbytes);
        if (!pcm)
            return HU_ERR_OUT_OF_MEMORY;
        for (size_t i = 0; i < nsamp; i++)
            pcm[i] = 0.0001f * (float)i;
        *pcm_out = pcm;
        *pcm_len = nbytes;
        return HU_OK;
    }
    if (!s->mock_flushed)
        return HU_ERR_TIMEOUT;
    s->mock_flushed = false;
    *recv_done = true;
    return HU_OK;
}

#else /* !HU_IS_TEST */

#include <stdlib.h>

#if defined(HU_GATEWAY_POSIX) && defined(HU_HAS_TLS)

static void json_set_str(hu_allocator_t *a, hu_json_value_t *obj, const char *key,
                         const char *val) {
    if (!val)
        val = "";
    hu_json_object_set(a, obj, key, hu_json_string_new(a, val, strlen(val)));
}

static hu_error_t build_generation_json(hu_allocator_t *alloc, const char *model_id,
                                        const char *voice_id, const char *context_id,
                                        const char *transcript, bool is_continue, bool flush,
                                        bool cancel,
                                        const hu_cartesia_voice_controls_t *voice_controls,
                                        char **out_json, size_t *out_len) {
    hu_json_value_t *o = hu_json_object_new(alloc);
    if (!o)
        return HU_ERR_OUT_OF_MEMORY;
    if (cancel) {
        if (context_id)
            json_set_str(alloc, o, "context_id", context_id);
        hu_json_object_set(alloc, o, "cancel", hu_json_bool_new(alloc, true));
    } else {
        json_set_str(alloc, o, "model_id", model_id ? model_id : "");
        json_set_str(alloc, o, "transcript", transcript ? transcript : "");
        hu_json_value_t *voice = hu_json_object_new(alloc);
        if (voice) {
            json_set_str(alloc, voice, "mode", "id");
            json_set_str(alloc, voice, "id", voice_id ? voice_id : "");
            if (voice_controls) {
                hu_json_value_t *ctrl = hu_json_object_new(alloc);
                if (ctrl) {
                    if (voice_controls->speed != 0.0f)
                        hu_json_object_set(alloc, ctrl, "speed",
                                           hu_json_number_new(alloc, voice_controls->speed));
                    if (voice_controls->emotion && voice_controls->emotion[0]) {
                        hu_json_value_t *emo_arr = hu_json_array_new(alloc);
                        if (emo_arr) {
                            hu_json_array_push(alloc, emo_arr,
                                               hu_json_string_new(alloc, voice_controls->emotion,
                                                                  strlen(voice_controls->emotion)));
                            hu_json_object_set(alloc, ctrl, "emotion", emo_arr);
                        }
                    }
                    hu_json_object_set(alloc, voice, "__experimental_controls", ctrl);
                }
            }
            hu_json_object_set(alloc, o, "voice", voice);
        }
        hu_json_value_t *fmt = hu_json_object_new(alloc);
        if (fmt) {
            json_set_str(alloc, fmt, "container", "raw");
            json_set_str(alloc, fmt, "encoding", "pcm_f32le");
            hu_json_object_set(alloc, fmt, "sample_rate", hu_json_number_new(alloc, 24000));
            hu_json_object_set(alloc, o, "output_format", fmt);
        }
        if (context_id)
            json_set_str(alloc, o, "context_id", context_id);
        hu_json_object_set(alloc, o, "continue", hu_json_bool_new(alloc, is_continue));
        if (flush)
            hu_json_object_set(alloc, o, "flush", hu_json_bool_new(alloc, true));
    }
    hu_error_t err = hu_json_stringify(alloc, o, out_json, out_len);
    hu_json_free(alloc, o);
    return err;
}

hu_error_t hu_cartesia_stream_open(hu_allocator_t *alloc, const char *api_key, const char *voice_id,
                                   const char *model_id, hu_cartesia_stream_t **out) {
    if (!alloc || !api_key || !api_key[0] || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_cartesia_stream_t *s =
        (hu_cartesia_stream_t *)alloc->alloc(alloc->ctx, sizeof(hu_cartesia_stream_t));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    if (voice_id && voice_id[0])
        (void)snprintf(s->voice_id, sizeof(s->voice_id), "%s", voice_id);
    else
        (void)snprintf(s->voice_id, sizeof(s->voice_id), "default");
    if (model_id && model_id[0])
        (void)snprintf(s->model_id, sizeof(s->model_id), "%s", model_id);
    else
        (void)snprintf(s->model_id, sizeof(s->model_id), "sonic-3-2026-01-12");

    char xhdr[512];
    int nh = snprintf(xhdr, sizeof(xhdr),
                      "Cartesia-Version: 2025-04-16\r\n"
                      "X-API-Key: %s\r\n",
                      api_key);
    if (nh < 0 || (size_t)nh >= sizeof(xhdr)) {
        alloc->free(alloc->ctx, s, sizeof(*s));
        return HU_ERR_INVALID_ARGUMENT;
    }

    static const char kurl[] = "wss://api.cartesia.ai/tts/websocket";
    hu_error_t err = hu_ws_connect_with_headers(alloc, kurl, xhdr, &s->ws);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, s, sizeof(*s));
        return err;
    }
    *out = s;
    return HU_OK;
}

void hu_cartesia_stream_close(hu_cartesia_stream_t *s, hu_allocator_t *alloc) {
    if (!s || !alloc)
        return;
    if (s->ws)
        hu_ws_client_free(s->ws, alloc);
    alloc->free(alloc->ctx, s, sizeof(*s));
}

static hu_error_t send_json(hu_cartesia_stream_t *s, const char *json, size_t json_len) {
    if (!s || !s->ws || !json)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_ws_send(s->ws, json, json_len);
}

hu_error_t hu_cartesia_stream_send_generation(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                              const char *context_id, const char *transcript,
                                              bool is_continue) {
    if (!s || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    char *json = NULL;
    size_t jl = 0;
    hu_error_t err = build_generation_json(
        alloc, s->model_id, s->voice_id, context_id, transcript, is_continue, false, false,
        s->has_voice_controls ? &s->voice_controls : NULL, &json, &jl);
    if (err != HU_OK)
        return err;
    err = send_json(s, json, jl);
    alloc->free(alloc->ctx, json, jl + 1);
    return err;
}

hu_error_t hu_cartesia_stream_flush_context(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                            const char *context_id) {
    if (!s || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    char *json = NULL;
    size_t jl = 0;
    hu_error_t err = build_generation_json(alloc, s->model_id, s->voice_id, context_id, "", false,
                                           true, false, NULL, &json, &jl);
    if (err != HU_OK)
        return err;
    err = send_json(s, json, jl);
    alloc->free(alloc->ctx, json, jl + 1);
    return err;
}

hu_error_t hu_cartesia_stream_cancel_context(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                             const char *context_id) {
    if (!s || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    char *json = NULL;
    size_t jl = 0;
    hu_error_t err = build_generation_json(alloc, s->model_id, s->voice_id, context_id, "", false,
                                           false, true, NULL, &json, &jl);
    if (err != HU_OK)
        return err;
    err = send_json(s, json, jl);
    alloc->free(alloc->ctx, json, jl + 1);
    return err;
}

hu_error_t hu_cartesia_stream_recv_next_wait(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                             int timeout_ms, void **pcm_out, size_t *pcm_len,
                                             bool *recv_done) {
    if (!s || !alloc || !pcm_out || !pcm_len || !recv_done)
        return HU_ERR_INVALID_ARGUMENT;
    *pcm_out = NULL;
    *pcm_len = 0;
    *recv_done = false;

    char *raw = NULL;
    size_t rl = 0;
    hu_error_t err = hu_ws_recv(s->ws, alloc, &raw, &rl, timeout_ms);
    if (err != HU_OK)
        return err;
    if (!raw || rl == 0) {
        if (raw)
            alloc->free(alloc->ctx, raw, rl);
        *recv_done = true;
        return HU_OK;
    }

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, raw, rl, &root) != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        alloc->free(alloc->ctx, raw, rl);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *type = hu_json_get_string(root, "type");
    if (type && strcmp(type, "done") == 0) {
        *recv_done = true;
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, raw, rl);
        return HU_OK;
    }
    if (type && strcmp(type, "error") == 0) {
        *recv_done = true;
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, raw, rl);
        return HU_ERR_IO;
    }
    if (type && strcmp(type, "chunk") == 0) {
        hu_json_value_t *data_v = hu_json_object_get(root, "data");
        if (data_v && data_v->type == HU_JSON_STRING && data_v->data.string.len > 0) {
            void *dec = NULL;
            size_t dec_len = 0;
            err = hu_multimodal_decode_base64(alloc, data_v->data.string.ptr,
                                              data_v->data.string.len, &dec, &dec_len);
            if (err == HU_OK && dec && dec_len > 0) {
                *pcm_out = dec;
                *pcm_len = dec_len;
            }
        }
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, raw, rl);
        return HU_OK;
    }

    hu_json_free(alloc, root);
    alloc->free(alloc->ctx, raw, rl);
    return HU_OK;
}

#else /* no TLS */

hu_error_t hu_cartesia_stream_open(hu_allocator_t *alloc, const char *api_key, const char *voice_id,
                                   const char *model_id, hu_cartesia_stream_t **out) {
    (void)api_key;
    (void)voice_id;
    (void)model_id;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_cartesia_stream_close(hu_cartesia_stream_t *s, hu_allocator_t *alloc) {
    (void)s;
    (void)alloc;
}

hu_error_t hu_cartesia_stream_send_generation(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                              const char *context_id, const char *transcript,
                                              bool is_continue) {
    (void)s;
    (void)alloc;
    (void)context_id;
    (void)transcript;
    (void)is_continue;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_cartesia_stream_flush_context(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                            const char *context_id) {
    (void)s;
    (void)alloc;
    (void)context_id;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_cartesia_stream_cancel_context(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                             const char *context_id) {
    (void)s;
    (void)alloc;
    (void)context_id;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_cartesia_stream_recv_next_wait(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                             int timeout_ms, void **pcm_out, size_t *pcm_len,
                                             bool *recv_done) {
    (void)timeout_ms;
    (void)s;
    (void)alloc;
    (void)pcm_out;
    (void)pcm_len;
    (void)recv_done;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_GATEWAY_POSIX && HU_HAS_TLS */
#endif /* !HU_IS_TEST */

hu_error_t hu_cartesia_stream_recv_next(hu_cartesia_stream_t *s, hu_allocator_t *alloc,
                                        void **pcm_out, size_t *pcm_len, bool *recv_done) {
    return hu_cartesia_stream_recv_next_wait(s, alloc, 30000, pcm_out, pcm_len, recv_done);
}
