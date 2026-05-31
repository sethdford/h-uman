#include "human/voice/realtime.h"
#include "human/core/json.h"
#include "human/core/privacy.h"
#include "human/core/string.h"
#include "human/multimodal.h"
#include "human/websocket/websocket.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

hu_error_t hu_voice_rt_session_create(hu_allocator_t *alloc, const hu_voice_rt_config_t *config,
                                      hu_voice_rt_session_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_voice_rt_session_t *s = alloc->alloc(alloc->ctx, sizeof(hu_voice_rt_session_t));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
#if HU_IS_TEST
    s->test_recv_seq = 0;
#endif
    if (config)
        s->config = *config;
    *out = s;
    return HU_OK;
}

hu_error_t hu_voice_rt_connect(hu_voice_rt_session_t *session) {
    if (!session)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    session->connected = true;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    const char *base = session->config.base_url && session->config.base_url[0]
                           ? session->config.base_url
                           : "wss://api.openai.com/v1/realtime";
    char url[512];
    int n;
    if (session->config.model && session->config.model[0])
        n = snprintf(url, sizeof(url), "%s?model=%s", base, session->config.model);
    else
        n = snprintf(url, sizeof(url), "%s", base);
    if (n <= 0 || (size_t)n >= sizeof(url))
        return HU_ERR_INVALID_ARGUMENT;

    const char *key = session->config.api_key ? session->config.api_key : "";
    for (const char *p = key; *p; p++) {
        if (*p == '\r' || *p == '\n')
            return HU_ERR_INVALID_ARGUMENT;
    }
    char headers[512];
    (void)snprintf(headers, sizeof(headers),
                   "Authorization: Bearer %s\r\nOpenAI-Beta: realtime=v1\r\n", key);

    hu_ws_client_t *ws = NULL;
    hu_error_t err = hu_ws_connect_with_headers(session->alloc, url, headers, &ws);
    if (err != HU_OK || !ws)
        return err;

    session->ws_client = ws;
    session->connected = true;

    {
        const char *mdl = (session->config.model && session->config.model[0])
                              ? session->config.model
                              : "gpt-4o-realtime-preview";
        const char *voice =
            (session->config.voice && session->config.voice[0]) ? session->config.voice : "alloy";
        static const char su_head[] = "{\"type\":\"session.update\",\"session\":{";
        static const char su_mid[] = ",\"modalities\":[\"text\",\"audio\"],\"voice\":";
        static const char su_tail[] =
            ",\"input_audio_format\":\"pcm16\","
            "\"output_audio_format\":\"pcm16\","
            "\"input_audio_transcription\":{\"model\":\"whisper-1\"},"
            "\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.5,"
            "\"prefix_padding_ms\":300,\"silence_duration_ms\":500}"
            "}}";
        hu_json_buf_t su_buf = {0};
        hu_error_t su_err = hu_json_buf_init(&su_buf, session->alloc);
        if (su_err == HU_OK)
            su_err = hu_json_buf_append_raw(&su_buf, su_head, sizeof(su_head) - 1);
        if (su_err == HU_OK)
            su_err = hu_json_append_key_value(&su_buf, "model", 5, mdl, strlen(mdl));
        if (su_err == HU_OK)
            su_err = hu_json_buf_append_raw(&su_buf, su_mid, sizeof(su_mid) - 1);
        if (su_err == HU_OK)
            su_err = hu_json_append_string(&su_buf, voice, strlen(voice));
        if (su_err == HU_OK)
            su_err = hu_json_buf_append_raw(&su_buf, su_tail, sizeof(su_tail) - 1);
        if (su_err == HU_OK)
            su_err = hu_ws_send(ws, su_buf.ptr, su_buf.len);
        hu_json_buf_free(&su_buf);
        if (su_err != HU_OK) {
            hu_ws_client_free(ws, session->alloc);
            session->ws_client = NULL;
            session->connected = false;
            return su_err;
        }
    }
    return HU_OK;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_voice_rt_send_audio(hu_voice_rt_session_t *session, const void *data,
                                  size_t data_len) {
    if (!session || !data)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)data_len;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!session->connected || !session->ws_client)
        return HU_ERR_IO;

    char *b64 = NULL;
    size_t b64_len = 0;
    hu_error_t err = hu_multimodal_encode_base64(session->alloc, data, data_len, &b64, &b64_len);
    if (err != HU_OK || !b64)
        return err;

    if (b64_len > SIZE_MAX / 2 - 64 || b64_len > 50U * 1024U * 1024U) {
        session->alloc->free(session->alloc->ctx, b64, b64_len + 1);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t json_cap = 64 + b64_len * 2;
    char *json = (char *)session->alloc->alloc(session->alloc->ctx, json_cap);
    if (!json) {
        session->alloc->free(session->alloc->ctx, b64, b64_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(json, json_cap, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}",
                     (int)b64_len, b64);
    session->alloc->free(session->alloc->ctx, b64, b64_len + 1);
    if (n <= 0 || (size_t)n >= json_cap) {
        session->alloc->free(session->alloc->ctx, json, json_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    err = hu_ws_send((hu_ws_client_t *)session->ws_client, json, (size_t)n);
    session->alloc->free(session->alloc->ctx, json, json_cap);
    return err;
#else
    (void)data_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_voice_rt_response_cancel(hu_voice_rt_session_t *session) {
    if (!session)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!session->connected || !session->ws_client)
        return HU_ERR_IO;
    static const char k_cancel[] = "{\"type\":\"response.cancel\"}";
    return hu_ws_send((hu_ws_client_t *)session->ws_client, k_cancel, sizeof(k_cancel) - 1);
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#if HU_IS_TEST || defined(HU_HTTP_CURL)
static void copy_event_type(const char *type, hu_voice_rt_event_t *out) {
    if (type) {
        strncpy(out->type, type, sizeof(out->type) - 1);
        out->type[sizeof(out->type) - 1] = '\0';
    }
}
#endif

hu_error_t hu_voice_rt_recv_event(hu_voice_rt_session_t *session, hu_allocator_t *alloc,
                                  hu_voice_rt_event_t *out, int timeout_ms) {
    if (!session || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
#if HU_IS_TEST
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    (void)timeout_ms;
    if (!session->connected)
        return HU_ERR_IO;
    unsigned call = session->test_recv_seq++;
    if ((call % 2u) == 0u) {
        copy_event_type("conversation.item.input_audio_transcription.completed", out);
        static const char k_mock_text[] = "Hello from realtime";
        size_t tlen = sizeof(k_mock_text) - 1u;
        out->transcript = hu_strndup(alloc, k_mock_text, tlen);
        if (!out->transcript)
            return HU_ERR_OUT_OF_MEMORY;
        out->transcript_len = tlen;
        out->done = false;
    } else {
        copy_event_type("response.audio.done", out);
        out->done = true;
    }
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->connected || !session->ws_client)
        return HU_ERR_IO;

    char *msg = NULL;
    size_t msg_len = 0;
    hu_error_t err =
        hu_ws_recv((hu_ws_client_t *)session->ws_client, alloc, &msg, &msg_len, timeout_ms);
    if (err != HU_OK || !msg) {
        if (msg)
            alloc->free(alloc->ctx, msg, msg_len + 1);
        return err;
    }

    hu_json_value_t *json = NULL;
    err = hu_json_parse(alloc, msg, msg_len, &json);
    if (err != HU_OK || !json) {
        alloc->free(alloc->ctx, msg, msg_len + 1);
        return err != HU_OK ? err : HU_ERR_IO;
    }

    const char *type = NULL;
    if (json->type == HU_JSON_OBJECT)
        type = hu_json_get_string(json, "type");
    copy_event_type(type, out);

    if (type) {
        if (strcmp(type, "response.audio.delta") == 0 ||
            strcmp(type, "response.output_audio.delta") == 0) {
            const char *delta = hu_json_get_string(json, "delta");
            if (delta && delta[0]) {
                size_t dlen = strlen(delta);
                out->audio_base64 = hu_strndup(alloc, delta, dlen);
                if (!out->audio_base64) {
                    hu_json_free(alloc, json);
                    alloc->free(alloc->ctx, msg, msg_len + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                out->audio_base64_len = dlen;
            }
        }

        if (strcmp(type, "response.audio_transcript.delta") == 0 ||
            strcmp(type, "response.output_audio_transcript.delta") == 0 ||
            strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
            const char *text = hu_json_get_string(json, "transcript");
            if (!text)
                text = hu_json_get_string(json, "delta");
            if (text && text[0]) {
                size_t tlen = strlen(text);
                out->transcript = hu_strndup(alloc, text, tlen);
                if (!out->transcript) {
                    hu_json_free(alloc, json);
                    alloc->free(alloc->ctx, msg, msg_len + 1);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                out->transcript_len = tlen;
            }
        }

        if (strcmp(type, "response.audio.done") == 0 || strcmp(type, "response.done") == 0 ||
            strcmp(type, "response.output_audio.done") == 0)
            out->done = true;
    }

    hu_json_free(alloc, json);
    alloc->free(alloc->ctx, msg, msg_len + 1);
    return HU_OK;
#else
    (void)alloc;
    (void)timeout_ms;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

void hu_voice_rt_event_free(hu_allocator_t *alloc, hu_voice_rt_event_t *event) {
    if (!alloc || !event)
        return;
    if (event->audio_base64)
        alloc->free(alloc->ctx, event->audio_base64, event->audio_base64_len + 1);
    if (event->transcript)
        alloc->free(alloc->ctx, event->transcript, event->transcript_len + 1);
    if (event->tool_call_id)
        alloc->free(alloc->ctx, event->tool_call_id, event->tool_call_id_len + 1);
    if (event->tool_name)
        alloc->free(alloc->ctx, event->tool_name, event->tool_name_len + 1);
    if (event->tool_args_json)
        alloc->free(alloc->ctx, event->tool_args_json, event->tool_args_json_len + 1);
    memset(event, 0, sizeof(*event));
}

hu_error_t hu_voice_rt_add_tool(hu_voice_rt_session_t *session, const char *name,
                                const char *description, const char *parameters_json) {
    if (!session || !name)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)description;
    (void)parameters_json;
    return HU_OK;
#elif defined(HU_HTTP_CURL)
    if (!session->connected || !session->ws_client)
        return HU_ERR_IO;
    const char *params = parameters_json && parameters_json[0] ? parameters_json : "{}";
    static const char k_sess_tool_head[] =
        "{\"type\":\"session.update\",\"session\":{\"tools\":[{\"type\":\"function\"";
    hu_json_buf_t buf = {0};
    hu_error_t err = hu_json_buf_init(&buf, session->alloc);
    if (err != HU_OK)
        return err;
    err = hu_json_buf_append_raw(&buf, k_sess_tool_head, sizeof(k_sess_tool_head) - 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_append_key_value(&buf, "name", 4, name, strlen(name));
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_append_key_value(&buf, "description", 11, description ? description : "",
                                   description ? strlen(description) : 0);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",\"parameters\":", 16);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, params, strlen(params));
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, "}]}}", 4);
    if (err != HU_OK)
        goto fail;
    err = hu_ws_send((hu_ws_client_t *)session->ws_client, buf.ptr, buf.len);
fail:
    hu_json_buf_free(&buf);
    return err;
#else
    (void)session;
    (void)name;
    (void)description;
    (void)parameters_json;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ── Voice Provider vtable wrapper ────────────────────────────────────── */

#include "human/voice/provider.h"

static hu_error_t openai_vp_connect(void *ctx) {
    return hu_voice_rt_connect((hu_voice_rt_session_t *)ctx);
}

static hu_error_t openai_vp_send_audio(void *ctx, const void *pcm16, size_t len) {
    return hu_voice_rt_send_audio((hu_voice_rt_session_t *)ctx, pcm16, len);
}

static hu_error_t openai_vp_recv_event(void *ctx, hu_allocator_t *alloc, hu_voice_rt_event_t *out,
                                       int timeout_ms) {
    return hu_voice_rt_recv_event((hu_voice_rt_session_t *)ctx, alloc, out, timeout_ms);
}

static hu_error_t openai_vp_add_tool(void *ctx, const char *name, const char *description,
                                     const char *parameters_json) {
    return hu_voice_rt_add_tool((hu_voice_rt_session_t *)ctx, name, description, parameters_json);
}

static hu_error_t openai_vp_cancel_response(void *ctx) {
    return hu_voice_rt_response_cancel((hu_voice_rt_session_t *)ctx);
}

static void openai_vp_disconnect(void *ctx, hu_allocator_t *alloc) {
    (void)alloc;
    hu_voice_rt_session_destroy((hu_voice_rt_session_t *)ctx);
}

static const char *openai_vp_get_name(void *ctx) {
    (void)ctx;
    return "openai_realtime";
}

static const hu_voice_provider_vtable_t openai_voice_vtable = {
    .connect = openai_vp_connect,
    .send_audio = openai_vp_send_audio,
    .recv_event = openai_vp_recv_event,
    .add_tool = openai_vp_add_tool,
    .cancel_response = openai_vp_cancel_response,
    .disconnect = openai_vp_disconnect,
    .get_name = openai_vp_get_name,
};

hu_error_t hu_voice_provider_openai_create(hu_allocator_t *alloc,
                                           const hu_voice_rt_config_t *config,
                                           hu_voice_provider_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    /* Privacy kill-switch: OpenAI Realtime streams microphone audio to the
     * cloud. Refuse construction under privacy mode — covers direct callers
     * (hu_voice_session_start in session.c) and the provider factory. */
    if (hu_privacy_enforced())
        return HU_ERR_NOT_SUPPORTED;
    hu_voice_rt_session_t *session = NULL;
    hu_error_t err = hu_voice_rt_session_create(alloc, config, &session);
    if (err != HU_OK)
        return err;
    out->ctx = session;
    out->vtable = &openai_voice_vtable;
    return HU_OK;
}

void hu_voice_rt_session_destroy(hu_voice_rt_session_t *session) {
    if (!session)
        return;
    hu_allocator_t *alloc = session->alloc;
#if defined(HU_HTTP_CURL) && !HU_IS_TEST
    if (session->ws_client) {
        hu_ws_client_free((hu_ws_client_t *)session->ws_client, alloc);
        session->ws_client = NULL;
    }
#endif
    if (session->session_id)
        alloc->free(alloc->ctx, session->session_id, strlen(session->session_id) + 1);
    alloc->free(alloc->ctx, session, sizeof(hu_voice_rt_session_t));
}
