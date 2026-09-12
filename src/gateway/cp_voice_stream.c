/*
 * Streaming voice session: binary mic chunks, STT, bus → agent, Cartesia TTS → binary PCM.
 * Micro-turn duplex FSM drives turn-taking and control-token stripping.
 */
#include "cp_internal.h"
#include "human/bus.h"
#include "human/config.h"
#include "human/core/time.h"
#include "human/gateway/voice_stream.h"
#include "human/multimodal.h"
#include "human/platform.h"
#include "human/tts/cartesia_stream.h"
#include "human/tts/transcript_prep.h"
#include "human/voice.h"
#include "human/voice/audio_emotion.h"
#include "human/voice/duplex.h"
#include "human/voice/emotion_voice_map.h"
#include "human/voice/provider.h"
#include "human/voice/semantic_eot.h"
#include "human/voice/turn_signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef HU_GATEWAY_POSIX

#include <time.h>

#define VS_MAX_SLOTS 8
#define VS_MAX_AUDIO (10u * 1024u * 1024u)

typedef struct {
    bool in_use;
    uint64_t conn_id;
    hu_ws_conn_t *conn;
    uint8_t *pcm_buf;
    size_t pcm_len;
    size_t pcm_cap;
    hu_cartesia_stream_t *tts;
    char session_key[HU_BUS_ID_LEN];
    char tts_context[96];
    bool tts_armed;
    unsigned turn_counter;
    char voice_id[128];
    char model_id[128];
    hu_duplex_session_t duplex;
    hu_voice_emotion_t current_emotion;
    hu_voice_params_t emotion_voice_params;
    hu_voice_provider_t provider;
    bool provider_active;
    bool provider_activity_started;
    volatile bool recv_pump_running;
    pthread_t recv_pump_tid;
    char provider_mode[32];
} vs_slot_t;

static vs_slot_t s_vs[VS_MAX_SLOTS];
static hu_control_protocol_t *s_proto;
static hu_bus_t *s_bus;
static vs_slot_t *s_active_tts_slot;

static int64_t vs_now_ms(void) {
    return (int64_t)hu_time_wall_ms();
}

typedef struct {
    vs_slot_t *slot;
    hu_allocator_t alloc;
} vs_recv_pump_ctx_t;

static void vs_pcm16_to_f32_send(vs_slot_t *sl, const void *pcm16, size_t pcm16_len) {
    if (!sl || !sl->conn || !sl->conn->active || !s_proto || !s_proto->ws)
        return;
    size_t n_samples = pcm16_len / 2;
    size_t f32_len = n_samples * sizeof(float);
    float *f32 = (float *)malloc(f32_len);
    if (!f32)
        return;
    const int16_t *src = (const int16_t *)pcm16;
    for (size_t i = 0; i < n_samples; i++)
        f32[i] = (float)src[i] / 32768.0f;
    (void)hu_ws_server_send_binary(s_proto->ws, sl->conn, (const char *)f32, f32_len);
    free(f32);
}

static void vs_emit_event(vs_slot_t *sl, const char *event, const char *payload) {
    if (!sl || !sl->conn || !sl->conn->active || !s_proto)
        return;
    hu_control_send_event_to_conn(s_proto, sl->conn, event, payload ? payload : "{}");
}

static void vs_emit_event_with_text(vs_slot_t *sl, hu_allocator_t *alloc, const char *event,
                                    const char *text) {
    if (!text || !text[0]) {
        vs_emit_event(sl, event, "{}");
        return;
    }
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return;
    cp_json_set_str(alloc, obj, "text", text);
    char *payload = NULL;
    size_t plen = 0;
    (void)hu_json_stringify(alloc, obj, &payload, &plen);
    hu_json_free(alloc, obj);
    if (payload) {
        vs_emit_event(sl, event, payload);
        alloc->free(alloc->ctx, payload, plen + 1);
    }
}

static void vs_emit_tool_call(vs_slot_t *sl, hu_allocator_t *alloc, hu_voice_rt_event_t *ev) {
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return;
    if (ev->tool_name)
        cp_json_set_str(alloc, obj, "name", ev->tool_name);
    if (ev->tool_call_id)
        cp_json_set_str(alloc, obj, "call_id", ev->tool_call_id);
    if (ev->tool_args_json)
        cp_json_set_str(alloc, obj, "args", ev->tool_args_json);
    char *payload = NULL;
    size_t plen = 0;
    (void)hu_json_stringify(alloc, obj, &payload, &plen);
    hu_json_free(alloc, obj);
    if (payload) {
        vs_emit_event(sl, "voice.tool_call", payload);
        alloc->free(alloc->ctx, payload, plen + 1);
    }
}

static void *vs_recv_pump_thread(void *arg) {
    vs_recv_pump_ctx_t *ctx = (vs_recv_pump_ctx_t *)arg;
    vs_slot_t *sl = ctx->slot;
    hu_allocator_t alloc = ctx->alloc;
    free(ctx);

    while (sl->recv_pump_running && sl->provider_active && sl->provider.vtable) {
        hu_voice_rt_event_t ev;
        memset(&ev, 0, sizeof(ev));
        hu_error_t err = sl->provider.vtable->recv_event(sl->provider.ctx, &alloc, &ev, 100);

        if (!sl->recv_pump_running)
            break;
        if (err != HU_OK) {
            hu_voice_rt_event_free(&alloc, &ev);
            if (err == HU_ERR_IO)
                break;
            continue;
        }
        if (!ev.type[0]) {
            hu_voice_rt_event_free(&alloc, &ev);
            continue;
        }

        if (strcmp(ev.type, "setupComplete") == 0)
            vs_emit_event(sl, "voice.setup_complete", "{}");
        else if (strcmp(ev.type, "sessionResumptionUpdate") == 0)
            vs_emit_event(sl, "voice.session_resumption", "{}");
        else if (strcmp(ev.type, "serverContent.inputTranscription") == 0)
            vs_emit_event_with_text(sl, &alloc, "voice.user.transcript", ev.transcript);
        else if (strcmp(ev.type, "serverContent.outputTranscription") == 0 ||
                 strcmp(ev.type, "response.audio_transcript.delta") == 0)
            vs_emit_event_with_text(sl, &alloc, "voice.assistant.transcript", ev.transcript);
        else if (strcmp(ev.type, "serverContent.modelTurn.audio") == 0 ||
                 strcmp(ev.type, "response.audio.delta") == 0) {
            if (ev.audio_base64 && ev.audio_base64_len > 0) {
                void *pcm = NULL;
                size_t pcm_len = 0;
                if (hu_multimodal_decode_base64(&alloc, ev.audio_base64, ev.audio_base64_len, &pcm,
                                                &pcm_len) == HU_OK &&
                    pcm && pcm_len > 0) {
                    vs_pcm16_to_f32_send(sl, pcm, pcm_len);
                    alloc.free(alloc.ctx, pcm, pcm_len);
                }
            }
        } else if (strcmp(ev.type, "serverContent.interrupted") == 0)
            vs_emit_event(sl, "voice.audio.interrupted", "{}");
        else if (strcmp(ev.type, "serverContent.generationComplete") == 0 ||
                 strcmp(ev.type, "response.done") == 0)
            vs_emit_event(sl, "voice.generation_complete", "{}");
        else if (strcmp(ev.type, "serverContent.turnComplete") == 0 ||
                 strcmp(ev.type, "response.audio.done") == 0)
            vs_emit_event(sl, "voice.audio.done", "{}");
        else if (strcmp(ev.type, "toolCall") == 0)
            vs_emit_tool_call(sl, &alloc, &ev);
        else if (strcmp(ev.type, "toolCallCancellation") == 0 && ev.transcript)
            vs_emit_event_with_text(sl, &alloc, "voice.tool_cancelled", ev.transcript);
        else if (strcmp(ev.type, "goAway") == 0) {
            char buf[64];
            (void)snprintf(buf, sizeof(buf), "{\"timeLeft\":%d}", ev.go_away_ms);
            vs_emit_event(sl, "voice.goaway", buf);
        } else if (ev.error)
            vs_emit_event_with_text(sl, &alloc, "voice.error",
                                    ev.transcript ? ev.transcript : "Provider error");

        if (strstr(ev.type, "speech_started"))
            vs_emit_event(sl, "voice.vad.speech_started", "{}");
        else if (strstr(ev.type, "speech_stopped"))
            vs_emit_event(sl, "voice.vad.speech_stopped", "{}");

        hu_voice_rt_event_free(&alloc, &ev);
    }
    return NULL;
}

static hu_error_t vs_start_recv_pump(vs_slot_t *sl, hu_allocator_t *alloc) {
    vs_recv_pump_ctx_t *ctx = (vs_recv_pump_ctx_t *)malloc(sizeof(vs_recv_pump_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->slot = sl;
    ctx->alloc = *alloc;
    sl->recv_pump_running = true;
    if (pthread_create(&sl->recv_pump_tid, NULL, vs_recv_pump_thread, ctx) != 0) {
        sl->recv_pump_running = false;
        free(ctx);
        return HU_ERR_IO;
    }
    return HU_OK;
}

static hu_allocator_t *vs_alloc_for_close(void) {
    if (s_proto && s_proto->alloc)
        return s_proto->alloc;
    static hu_allocator_t sys;
    static bool sys_init;
    if (!sys_init) {
        sys = hu_system_allocator();
        sys_init = true;
    }
    return &sys;
}

static const char *vs_mime_ext(const char *mime) {
    if (!mime)
        return ".webm";
    if (strstr(mime, "wav"))
        return ".wav";
    if (strstr(mime, "mp3") || strstr(mime, "mpeg"))
        return ".mp3";
    if (strstr(mime, "ogg"))
        return ".ogg";
    return ".webm";
}

static vs_slot_t *vs_find_slot_by_conn(const hu_ws_conn_t *c) {
    if (!c)
        return NULL;
    for (int i = 0; i < VS_MAX_SLOTS; i++) {
        if (s_vs[i].in_use && s_vs[i].conn_id == c->id)
            return &s_vs[i];
    }
    return NULL;
}

static vs_slot_t *vs_alloc_slot(hu_ws_conn_t *c) {
    vs_slot_t *ex = vs_find_slot_by_conn(c);
    if (ex)
        return ex;
    for (int i = 0; i < VS_MAX_SLOTS; i++) {
        if (!s_vs[i].in_use) {
            memset(&s_vs[i], 0, sizeof(s_vs[i]));
            s_vs[i].in_use = true;
            s_vs[i].conn_id = c->id;
            s_vs[i].conn = c;
            memcpy(s_vs[i].session_key, "voice", 5);
            s_vs[i].session_key[5] = '\0';
            hu_duplex_session_init(&s_vs[i].duplex);
            return &s_vs[i];
        }
    }
    return NULL;
}

static void vs_free_slot(vs_slot_t *sl, hu_allocator_t *alloc) {
    if (!sl)
        return;
    if (s_active_tts_slot == sl)
        s_active_tts_slot = NULL;
    if (sl->recv_pump_running) {
        sl->recv_pump_running = false;
        (void)pthread_join(sl->recv_pump_tid, NULL);
    }
    if (sl->provider_active && sl->provider.vtable) {
        sl->provider.vtable->disconnect(sl->provider.ctx, alloc);
        sl->provider_active = false;
    }
    if (sl->tts) {
        hu_cartesia_stream_close(sl->tts, alloc);
        sl->tts = NULL;
    }
    if (sl->pcm_buf) {
        alloc->free(alloc->ctx, sl->pcm_buf, sl->pcm_cap);
        sl->pcm_buf = NULL;
        sl->pcm_len = 0;
        sl->pcm_cap = 0;
    }
    memset(sl, 0, sizeof(*sl));
}

static void vs_drain_tts_to_conn(vs_slot_t *sl) {
    if (!sl || !sl->tts || !s_proto || !s_proto->ws || !sl->conn || !sl->conn->active)
        return;
    hu_allocator_t *a = s_proto->alloc;
    for (;;) {
        void *pcm = NULL;
        size_t n = 0;
        bool done = false;
        hu_error_t err = hu_cartesia_stream_recv_next(sl->tts, a, &pcm, &n, &done);
        if (err != HU_OK)
            break;
        if (pcm && n > 0)
            (void)hu_ws_server_send_binary(s_proto->ws, sl->conn, (const char *)pcm, n);
        if (pcm)
            a->free(a->ctx, pcm, n);
        if (done)
            break;
    }
}

static void vs_finish_agent_turn(vs_slot_t *sl, hu_allocator_t *a) {
    (void)hu_cartesia_stream_flush_context(sl->tts, a, sl->tts_context);
    vs_drain_tts_to_conn(sl);
    hu_control_send_event_to_conn(s_proto, sl->conn, "voice.audio.done", "{}");
    sl->tts_armed = false;
    if (s_active_tts_slot == sl)
        s_active_tts_slot = NULL;
}

static bool vs_bus_cb(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx) {
    (void)user_ctx;
    if (!s_proto || !s_active_tts_slot || !s_active_tts_slot->tts_armed || !s_active_tts_slot->tts)
        return true;
    if (!ev || strcmp(ev->id, s_active_tts_slot->session_key) != 0)
        return true;

    hu_allocator_t *a = s_proto->alloc;
    vs_slot_t *sl = s_active_tts_slot;
    int64_t now = vs_now_ms();

    switch (type) {
    case HU_BUS_THINKING_CHUNK:
        /* Model reasoning; never feed to Cartesia or advance duplex from this stream. */
        return true;

    case HU_BUS_TOOL_CALL:
        /* Tool invocation metadata only; do not speak or disturb TTS context. */
        return true;

    case HU_BUS_TOOL_CALL_RESULT:
        /* Raw tool output; spoken text arrives in subsequent HU_BUS_MESSAGE_CHUNK. */
        return true;

    case HU_BUS_MESSAGE_CHUNK: {
        const char *tok = ev->message[0] ? ev->message : NULL;
        if (!tok || !tok[0])
            return true;

        size_t tok_len = strlen(tok);
        hu_turn_signal_result_t sig;
        hu_turn_signal_extract(tok, tok_len, &sig);

        hu_turn_signal_t fsm_signal = sig.had_token ? sig.signal : HU_TURN_SIGNAL_CONTINUE;
        hu_turn_action_t action;
        hu_duplex_agent_chunk(&sl->duplex, now, fsm_signal, &action);

        if (action == HU_TURN_ACTION_FLUSH_AUDIO) {
            char stripped[HU_BUS_MSG_LEN];
            const char *tts_text = tok;
            if (sig.had_token) {
                size_t slen = hu_turn_signal_strip(tok, tok_len, stripped, sizeof(stripped));
                if (slen > 0)
                    tts_text = stripped;
                else
                    return true;
            }
            /* Emotion-aware TTS: detect emotion and apply voice controls to Cartesia */
            {
                hu_voice_emotion_t emo = HU_VOICE_EMOTION_NEUTRAL;
                float emo_conf = 0.0f;
                if (hu_emotion_detect_from_text(tts_text, strlen(tts_text), &emo, &emo_conf) ==
                        HU_OK &&
                    emo_conf > 0.3f && emo != sl->current_emotion) {
                    sl->current_emotion = emo;
                    sl->emotion_voice_params = hu_emotion_voice_map(emo);
                    static const char *const cartesia_emotions[] = {
                        [HU_VOICE_EMOTION_NEUTRAL] = NULL,
                        [HU_VOICE_EMOTION_JOY] = "positivity:high",
                        [HU_VOICE_EMOTION_SADNESS] = "sadness:high",
                        [HU_VOICE_EMOTION_EMPATHY] = "sadness:low",
                        [HU_VOICE_EMOTION_EXCITEMENT] = "surprise:high",
                        [HU_VOICE_EMOTION_CONCERN] = "curiosity:high",
                        [HU_VOICE_EMOTION_CALM] = "positivity:low",
                        [HU_VOICE_EMOTION_URGENCY] = "anger:low",
                    };
                    hu_cartesia_voice_controls_t ctrl = {0};
                    ctrl.speed = sl->emotion_voice_params.rate_factor - 1.0f;
                    ctrl.emotion_intensity = sl->emotion_voice_params.emphasis;
                    if ((size_t)emo < sizeof(cartesia_emotions) / sizeof(cartesia_emotions[0]))
                        ctrl.emotion = cartesia_emotions[emo];
                    hu_cartesia_stream_set_voice_controls(sl->tts, &ctrl);
                }
            }
            if (hu_cartesia_stream_send_generation(sl->tts, a, sl->tts_context, tts_text, true) ==
                HU_OK)
                vs_drain_tts_to_conn(sl);
        } else if (action == HU_TURN_ACTION_YIELD_FLOOR) {
            vs_finish_agent_turn(sl, a);
        }
        return true;
    }

    case HU_BUS_MESSAGE_SENT: {
        hu_turn_action_t action;
        hu_duplex_agent_chunk(&sl->duplex, now, HU_TURN_SIGNAL_YIELD, &action);
        vs_finish_agent_turn(sl, a);
        return true;
    }

    default:
        /* Other bus events (e.g. MESSAGE_RECEIVED, ERROR): no voice/TTS side effects. */
        return true;
    }
}

void hu_voice_stream_attach_bus(hu_bus_t *bus, hu_control_protocol_t *proto) {
    s_bus = bus;
    s_proto = proto;
    if (bus)
        (void)hu_bus_subscribe(bus, vs_bus_cb, NULL, HU_BUS_EVENT_COUNT);
}

void hu_voice_stream_detach_bus(hu_bus_t *bus) {
    if (bus && s_bus == bus) {
        hu_bus_unsubscribe(bus, vs_bus_cb, NULL);
        s_bus = NULL;
        s_proto = NULL;
        s_active_tts_slot = NULL;
    }
}

void hu_voice_stream_on_binary(hu_control_protocol_t *proto, hu_ws_conn_t *conn, const char *data,
                               size_t data_len) {
    if (!proto || !proto->alloc || !conn || !data || data_len == 0)
        return;
    vs_slot_t *sl = vs_find_slot_by_conn(conn);
    if (!sl)
        return;
    if (sl->provider_active && sl->provider.vtable && sl->provider.vtable->send_audio) {
        if (!sl->provider_activity_started && sl->provider.vtable->send_activity_start) {
            (void)sl->provider.vtable->send_activity_start(sl->provider.ctx);
            sl->provider_activity_started = true;
            vs_emit_event(sl, "voice.vad.speech_started", "{}");
        }
        (void)sl->provider.vtable->send_audio(sl->provider.ctx, data, data_len);
        return;
    }
    hu_allocator_t *alloc = proto->alloc;
    if (sl->pcm_len + data_len > VS_MAX_AUDIO)
        return;
    if (sl->pcm_len + data_len > sl->pcm_cap) {
        size_t ncap = sl->pcm_cap ? sl->pcm_cap * 2u : 65536u;
        while (ncap < sl->pcm_len + data_len)
            ncap *= 2u;
        if (ncap > VS_MAX_AUDIO)
            ncap = VS_MAX_AUDIO;
        uint8_t *nb = (uint8_t *)alloc->alloc(alloc->ctx, ncap);
        if (!nb)
            return;
        if (sl->pcm_buf && sl->pcm_len)
            memcpy(nb, sl->pcm_buf, sl->pcm_len);
        if (sl->pcm_buf)
            alloc->free(alloc->ctx, sl->pcm_buf, sl->pcm_cap);
        sl->pcm_buf = nb;
        sl->pcm_cap = ncap;
    }
    memcpy(sl->pcm_buf + sl->pcm_len, data, data_len);
    sl->pcm_len += data_len;
}

void hu_voice_stream_on_conn_close(hu_ws_conn_t *conn) {
    vs_slot_t *sl = vs_find_slot_by_conn(conn);
    if (!sl)
        return;
    vs_free_slot(sl, vs_alloc_for_close());
}

hu_error_t cp_voice_session_start(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!alloc || !app || !app->config || !conn || !proto || !root)
        return HU_ERR_INVALID_ARGUMENT;

    vs_slot_t *sl = vs_alloc_slot(conn);
    if (!sl)
        return HU_ERR_ALREADY_EXISTS;

    const hu_config_t *cfg = (const hu_config_t *)app->config;
    hu_json_value_t *params = hu_json_object_get((hu_json_value_t *)root, "params");

    const char *mode_str = NULL;
    const char *api_key_override = NULL;
    const char *voice_id = NULL;
    const char *model_id = NULL;
    if (params) {
        mode_str = hu_json_get_string(params, "mode");
        const char *ak = hu_json_get_string(params, "apiKey");
        if (ak && ak[0])
            api_key_override = ak;
        const char *v = hu_json_get_string(params, "voiceId");
        if (v)
            voice_id = v;
        const char *m = hu_json_get_string(params, "modelId");
        if (m)
            model_id = m;
    }

    bool sts = mode_str && mode_str[0] &&
               (strcmp(mode_str, "gemini_live") == 0 || strcmp(mode_str, "openai_realtime") == 0 ||
                strcmp(mode_str, "realtime") == 0);

    if (sts) {
        if (sl->tts) {
            hu_cartesia_stream_close(sl->tts, alloc);
            sl->tts = NULL;
        }
        if (sl->recv_pump_running) {
            sl->recv_pump_running = false;
            (void)pthread_join(sl->recv_pump_tid, NULL);
        }
        if (sl->provider_active && sl->provider.vtable) {
            sl->provider.vtable->disconnect(sl->provider.ctx, alloc);
            sl->provider_active = false;
            memset(&sl->provider, 0, sizeof(sl->provider));
        }

        hu_voice_provider_extras_t extras = {0};
        if (api_key_override)
            extras.api_key = api_key_override;
        if (voice_id && voice_id[0])
            extras.voice_id = voice_id;
        if (model_id && model_id[0])
            extras.model_id = model_id;

        hu_voice_provider_t vp;
        hu_error_t perr = hu_voice_provider_create_from_config(alloc, cfg, mode_str, &extras, &vp);
        if (perr != HU_OK)
            return perr;
        if (!vp.vtable || !vp.vtable->connect) {
            return HU_ERR_NOT_SUPPORTED;
        }
        perr = vp.vtable->connect(vp.ctx);
        if (perr != HU_OK) {
            if (vp.vtable->disconnect)
                vp.vtable->disconnect(vp.ctx, alloc);
            return perr;
        }
        sl->provider = vp;
        sl->provider_active = true;
        sl->provider_activity_started = false;
        (void)snprintf(sl->provider_mode, sizeof(sl->provider_mode), "%s", mode_str);
        perr = vs_start_recv_pump(sl, alloc);
        if (perr != HU_OK) {
            sl->provider.vtable->disconnect(sl->provider.ctx, alloc);
            sl->provider_active = false;
            memset(&sl->provider, 0, sizeof(sl->provider));
            return perr;
        }

        int in_sr = (strcmp(mode_str, "gemini_live") == 0) ? 16000 : 24000;
        hu_json_value_t *res = hu_json_object_new(alloc);
        if (!res) {
            sl->recv_pump_running = false;
            (void)pthread_join(sl->recv_pump_tid, NULL);
            sl->provider.vtable->disconnect(sl->provider.ctx, alloc);
            sl->provider_active = false;
            memset(&sl->provider, 0, sizeof(sl->provider));
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
        cp_json_set_str(alloc, res, "mode", mode_str);
        cp_json_set_str(alloc, res, "input_encoding", "pcm_s16le");
        cp_json_set_str(alloc, res, "output_encoding", "pcm_f32le");
        hu_json_object_set(alloc, res, "input_sample_rate", hu_json_number_new(alloc, in_sr));
        hu_json_object_set(alloc, res, "output_sample_rate", hu_json_number_new(alloc, 24000));
        char sid[40];
        (void)snprintf(sid, sizeof(sid), "%llu", (unsigned long long)conn->id);
        cp_json_set_str(alloc, res, "sessionId", sid);
        hu_error_t err = hu_json_stringify(alloc, res, out, out_len);
        hu_json_free(alloc, res);
        return err;
    }

    const char *key = hu_config_get_provider_key(cfg, "cartesia");
    if (api_key_override)
        key = api_key_override;
    if (!key || !key[0])
        key = getenv("CARTESIA_API_KEY");
    if (!key || !key[0])
        return HU_ERR_GATEWAY_AUTH;

    if (!voice_id || !voice_id[0])
        voice_id = cfg->voice.tts_voice;
    if (!voice_id || !voice_id[0])
        voice_id = getenv("FERNI_VOICE_ID");
    if (!model_id || !model_id[0])
        model_id = cfg->voice.tts_model;
    if (!model_id || !model_id[0])
        model_id = getenv("CARTESIA_MODEL");

    if (sl->tts) {
        hu_cartesia_stream_close(sl->tts, alloc);
        sl->tts = NULL;
    }

    hu_error_t oerr = hu_cartesia_stream_open(alloc, key, voice_id ? voice_id : "",
                                              model_id ? model_id : "", &sl->tts);
    if (oerr != HU_OK)
        return oerr;

    if (voice_id && voice_id[0])
        (void)snprintf(sl->voice_id, sizeof(sl->voice_id), "%s", voice_id);
    if (model_id && model_id[0])
        (void)snprintf(sl->model_id, sizeof(sl->model_id), "%s", model_id);

    hu_json_value_t *res = hu_json_object_new(alloc);
    if (!res)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, res, "encoding", "pcm_f32le");
    hu_json_object_set(alloc, res, "sampleRate", hu_json_number_new(alloc, 24000));
    char sid[40];
    (void)snprintf(sid, sizeof(sid), "%llu", (unsigned long long)conn->id);
    cp_json_set_str(alloc, res, "sessionId", sid);
    hu_error_t err = hu_json_stringify(alloc, res, out, out_len);
    hu_json_free(alloc, res);
    return err;
}

hu_error_t cp_voice_session_stop(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)proto;
    (void)root;
    *out = NULL;
    *out_len = 0;
    if (!alloc || !app || !conn)
        return HU_ERR_INVALID_ARGUMENT;
    vs_slot_t *sl = vs_find_slot_by_conn(conn);
    if (sl)
        vs_free_slot(sl, alloc);
    hu_json_value_t *res = hu_json_object_new(alloc);
    if (!res)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
    hu_error_t err = hu_json_stringify(alloc, res, out, out_len);
    hu_json_free(alloc, res);
    return err;
}

hu_error_t cp_voice_session_interrupt(hu_allocator_t *alloc, hu_app_context_t *app,
                                      hu_ws_conn_t *conn, const hu_control_protocol_t *proto,
                                      const hu_json_value_t *root, char **out, size_t *out_len) {
    (void)proto;
    (void)root;
    *out = NULL;
    *out_len = 0;
    if (!alloc || !app || !conn)
        return HU_ERR_INVALID_ARGUMENT;
    vs_slot_t *sl = vs_find_slot_by_conn(conn);
    if (sl && sl->provider_active && sl->provider.vtable) {
        if (sl->provider.vtable->cancel_response)
            (void)sl->provider.vtable->cancel_response(sl->provider.ctx);
        if (sl->provider_activity_started && sl->provider.vtable->send_activity_end)
            (void)sl->provider.vtable->send_activity_end(sl->provider.ctx);
        sl->provider_activity_started = false;
    }
    if (sl && !sl->provider_active) {
        hu_turn_action_t action;
        hu_duplex_user_chunk(&sl->duplex, vs_now_ms(), HU_TURN_SIGNAL_INTERRUPT, &action);
    }
    if (sl && !sl->provider_active && sl->tts && sl->tts_context[0])
        (void)hu_cartesia_stream_cancel_context(sl->tts, alloc, sl->tts_context);
    if (sl) {
        sl->pcm_len = 0;
        sl->tts_armed = false;
        if (s_active_tts_slot == sl)
            s_active_tts_slot = NULL;
    }
    if (sl && conn->active)
        hu_control_send_event_to_conn((hu_control_protocol_t *)proto, conn,
                                      "voice.audio.interrupted", "{}");

    hu_json_value_t *res = hu_json_object_new(alloc);
    if (!res)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
    hu_error_t err = hu_json_stringify(alloc, res, out, out_len);
    hu_json_free(alloc, res);
    return err;
}

hu_error_t cp_voice_audio_end(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                              const hu_control_protocol_t *proto, const hu_json_value_t *root,
                              char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!alloc || !app || !app->config || !conn || !proto)
        return HU_ERR_INVALID_ARGUMENT;

    vs_slot_t *sl = vs_find_slot_by_conn(conn);
    if (!sl)
        return HU_ERR_INVALID_ARGUMENT;

    if (sl->provider_active && sl->provider.vtable) {
        if (sl->provider_activity_started) {
            if (sl->provider.vtable->send_activity_end)
                (void)sl->provider.vtable->send_activity_end(sl->provider.ctx);
            sl->provider_activity_started = false;
            vs_emit_event(sl, "voice.vad.speech_stopped", "{}");
        }
        if (sl->provider.vtable->send_audio_stream_end)
            (void)sl->provider.vtable->send_audio_stream_end(sl->provider.ctx);
        hu_json_value_t *res = hu_json_object_new(alloc);
        if (!res)
            return HU_ERR_OUT_OF_MEMORY;
        hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
        hu_error_t err = hu_json_stringify(alloc, res, out, out_len);
        hu_json_free(alloc, res);
        return err;
    }

    if (!sl->tts)
        return HU_ERR_INVALID_ARGUMENT;
    if (sl->pcm_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *mime_type = "audio/webm";
    const char *session_key = sl->session_key;
    hu_json_value_t *params = hu_json_object_get((hu_json_value_t *)root, "params");
    if (params) {
        const char *m = hu_json_get_string(params, "mimeType");
        if (m && m[0])
            mime_type = m;
        const char *sk = hu_json_get_string(params, "sessionKey");
        if (sk && sk[0]) {
            size_t l = strlen(sk);
            if (l >= sizeof(sl->session_key))
                l = sizeof(sl->session_key) - 1;
            memcpy(sl->session_key, sk, l);
            sl->session_key[l] = '\0';
            session_key = sl->session_key;
        }
    }

    hu_voice_config_t voice_cfg = {0};
    (void)hu_voice_config_from_settings((const hu_config_t *)app->config, &voice_cfg);
    voice_cfg.language = NULL;

    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir)
        return HU_ERR_IO;
    char tmpl[512];
    (void)snprintf(tmpl, sizeof(tmpl), "%s/human_vs_XXXXXX%s", tmp_dir, vs_mime_ext(mime_type));
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);

    int fd = mkstemps(tmpl, (int)strlen(vs_mime_ext(mime_type)));
    if (fd < 0) {
        return HU_ERR_IO;
    }
    {
        const uint8_t *p = sl->pcm_buf;
        size_t left = sl->pcm_len;
        while (left > 0) {
            ssize_t w = write(fd, p, left > 65536 ? 65536 : left);
            if (w <= 0) {
                close(fd);
                unlink(tmpl);
                return HU_ERR_IO;
            }
            p += (size_t)w;
            left -= (size_t)w;
        }
    }
    close(fd);

    char *text = NULL;
    size_t text_len = 0;
    hu_error_t err = hu_voice_stt_file(alloc, &voice_cfg, tmpl, &text, &text_len);
    unlink(tmpl);

    /* Audio features: only extract from raw PCM (audio/pcm, audio/l16, audio/wav).
     * Encoded formats (webm, ogg, mp3) are NOT decodable int16 PCM — feeding them
     * to hu_audio_features_extract produces garbage energy/pitch values. */
    hu_audio_features_t feats;
    memset(&feats, 0, sizeof(feats));
    bool is_raw_pcm = (strstr(mime_type, "pcm") != NULL || strstr(mime_type, "l16") != NULL ||
                       strstr(mime_type, "wav") != NULL);
    if (is_raw_pcm && sl->pcm_len >= 320 && (sl->pcm_len % 2u) == 0u && sl->pcm_buf) {
        (void)hu_audio_features_extract((const int16_t *)sl->pcm_buf, sl->pcm_len / 2u, 16000u,
                                        &feats);
    }
    sl->pcm_len = 0;

    if (err != HU_OK || !text)
        return err == HU_OK ? HU_ERR_IO : err;

    /* Fuse text + audio emotion for TTS voice selection */
    {
        hu_voice_emotion_t text_emo = HU_VOICE_EMOTION_NEUTRAL;
        float text_conf = 0.0f;
        (void)hu_emotion_detect_from_text(text, text_len, &text_emo, &text_conf);
        hu_voice_emotion_t audio_emo = HU_VOICE_EMOTION_NEUTRAL;
        float audio_conf = 0.0f;
        if (feats.valid)
            (void)hu_audio_emotion_classify(&feats, &audio_emo, &audio_conf);
        hu_voice_emotion_t fused_emo = HU_VOICE_EMOTION_NEUTRAL;
        float fused_conf = 0.0f;
        (void)hu_emotion_fuse(text_emo, text_conf, audio_emo, audio_conf, &fused_emo, &fused_conf);
        if (fused_conf > 0.25f) {
            sl->current_emotion = fused_emo;
            sl->emotion_voice_params = hu_emotion_voice_map(fused_emo);
        }
    }

    /* Semantic EOT: transcript + acoustic features */
    hu_turn_signal_t eot_signal = HU_TURN_SIGNAL_YIELD;
    {
        hu_semantic_eot_config_t eot_cfg;
        hu_semantic_eot_config_default(&eot_cfg);
        hu_semantic_eot_result_t eot_result;
        memset(&eot_result, 0, sizeof(eot_result));
        float pitch_delta = feats.valid ? (feats.pitch_mean_hz - 120.0f) : 0.0f;
        hu_semantic_eot_classifier_t eot_cls;
        hu_semantic_eot_classifier_default(&eot_cls);
        float eot_energy = feats.valid ? feats.energy_db : -40.0f;
        hu_error_t eot_err = hu_semantic_eot_classify(&eot_cls, &eot_cfg, text, text_len, 0,
                                                      eot_energy, pitch_delta, &eot_result);
        if (eot_err == HU_OK && eot_result.is_endpoint)
            eot_signal = eot_result.suggested_signal;
    }

    hu_turn_action_t turn_action;
    hu_duplex_user_chunk(&sl->duplex, vs_now_ms(), eot_signal, &turn_action);
    hu_duplex_start_streaming(&sl->duplex, vs_now_ms());

    /* Targeted transcript event */
    hu_json_value_t *tev = hu_json_object_new(alloc);
    if (!tev) {
        alloc->free(alloc->ctx, text, text_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    cp_json_set_str(alloc, tev, "text", text);
    char *tpayload = NULL;
    size_t tplen = 0;
    (void)hu_json_stringify(alloc, tev, &tpayload, &tplen);
    hu_json_free(alloc, tev);
    if (tpayload)
        hu_control_send_event_to_conn((hu_control_protocol_t *)proto, conn, "voice.transcript",
                                      tpayload);
    alloc->free(alloc->ctx, tpayload, tplen + 1);

    (void)snprintf(sl->tts_context, sizeof(sl->tts_context), "ctx-%llu-%u",
                   (unsigned long long)conn->id, ++sl->turn_counter);
    sl->tts_armed = true;
    s_active_tts_slot = sl;

    hu_bus_event_t bev;
    memset(&bev, 0, sizeof(bev));
    bev.type = HU_BUS_MESSAGE_RECEIVED;
    (void)snprintf(bev.channel, sizeof(bev.channel), "control-ui");
    (void)snprintf(bev.id, sizeof(bev.id), "%s", session_key);
    bev.payload = text;
    size_t tl = strlen(text);
    if (tl >= HU_BUS_MSG_LEN)
        tl = HU_BUS_MSG_LEN - 1;
    memcpy(bev.message, text, tl);
    bev.message[tl] = '\0';
    hu_bus_publish(app->bus, &bev);
    alloc->free(alloc->ctx, text, text_len + 1);

    hu_json_value_t *res = hu_json_object_new(alloc);
    if (!res)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
    err = hu_json_stringify(alloc, res, out, out_len);
    hu_json_free(alloc, res);
    return err;
}

hu_error_t cp_voice_tool_response(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)proto;
    *out = NULL;
    *out_len = 0;
    if (!alloc || !app || !conn || !root)
        return HU_ERR_INVALID_ARGUMENT;
    hu_json_value_t *params = hu_json_object_get((hu_json_value_t *)root, "params");
    if (!params)
        return HU_ERR_INVALID_ARGUMENT;
    const char *name = hu_json_get_string(params, "name");
    const char *call_id = hu_json_get_string(params, "call_id");
    if (!call_id)
        call_id = hu_json_get_string(params, "callId");
    const char *response = hu_json_get_string(params, "result");
    if (!response)
        response = hu_json_get_string(params, "response");
    if (!name || !call_id || !response)
        return HU_ERR_INVALID_ARGUMENT;
    vs_slot_t *sl = vs_find_slot_by_conn(conn);
    if (sl && sl->provider_active && sl->provider.vtable && sl->provider.vtable->send_tool_response)
        (void)sl->provider.vtable->send_tool_response(sl->provider.ctx, name, call_id, response);
    hu_json_value_t *res = hu_json_object_new(alloc);
    if (!res)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
    hu_error_t err = hu_json_stringify(alloc, res, out, out_len);
    hu_json_free(alloc, res);
    return err;
}

hu_error_t cp_voice_validate(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                             const hu_control_protocol_t *proto, const hu_json_value_t *root,
                             char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    (void)conn;
    (void)proto;
    if (!alloc || !app || !app->config || !root)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_config_t *cfg = (const hu_config_t *)app->config;
    hu_json_value_t *params = hu_json_object_get((hu_json_value_t *)root, "params");
    const char *mode = params ? hu_json_get_string(params, "mode") : NULL;
    if (!mode || !mode[0])
        mode = "gemini_live";
    const char *api_key = params ? hu_json_get_string(params, "apiKey") : NULL;

    hu_voice_provider_extras_t extras = {0};
    if (api_key && api_key[0])
        extras.api_key = api_key;

    hu_voice_provider_t provider = {0};
    hu_error_t verr = hu_voice_provider_create_from_config(alloc, cfg, mode, &extras, &provider);
    if (verr != HU_OK) {
        hu_json_value_t *vres = hu_json_object_new(alloc);
        if (!vres)
            return HU_ERR_OUT_OF_MEMORY;
        hu_json_object_set(alloc, vres, "ok", hu_json_bool_new(alloc, false));
        const char *msg = (verr == HU_ERR_GATEWAY_AUTH)
                              ? "Missing API key — check your provider credentials"
                          : (verr == HU_ERR_NOT_SUPPORTED) ? "Unsupported voice mode"
                                                           : "Provider configuration error";
        cp_json_set_str(alloc, vres, "error", msg);
        hu_error_t jerr = hu_json_stringify(alloc, vres, out, out_len);
        hu_json_free(alloc, vres);
        return jerr;
    }

    verr = provider.vtable->connect(provider.ctx);
    if (verr != HU_OK) {
        provider.vtable->disconnect(provider.ctx, alloc);
        hu_json_value_t *vres = hu_json_object_new(alloc);
        if (!vres)
            return HU_ERR_OUT_OF_MEMORY;
        hu_json_object_set(alloc, vres, "ok", hu_json_bool_new(alloc, false));
        cp_json_set_str(alloc, vres, "error", "Connection failed — check your API key and network");
        hu_error_t jerr = hu_json_stringify(alloc, vres, out, out_len);
        hu_json_free(alloc, vres);
        return jerr;
    }

    provider.vtable->disconnect(provider.ctx, alloc);

    hu_json_value_t *res = hu_json_object_new(alloc);
    if (!res)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, res, "ok", hu_json_bool_new(alloc, true));
    cp_json_set_str(alloc, res, "mode", mode);
    hu_error_t jerr = hu_json_stringify(alloc, res, out, out_len);
    hu_json_free(alloc, res);
    return jerr;
}

#else /* !HU_GATEWAY_POSIX */

void hu_voice_stream_attach_bus(hu_bus_t *bus, hu_control_protocol_t *proto) {
    (void)bus;
    (void)proto;
}

void hu_voice_stream_detach_bus(hu_bus_t *bus) {
    (void)bus;
}

void hu_voice_stream_on_binary(hu_control_protocol_t *proto, hu_ws_conn_t *conn, const char *data,
                               size_t data_len) {
    (void)proto;
    (void)conn;
    (void)data;
    (void)data_len;
}

void hu_voice_stream_on_conn_close(hu_ws_conn_t *conn) {
    (void)conn;
}

hu_error_t cp_voice_tool_response(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                  const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                  char **out, size_t *out_len) {
    (void)alloc;
    (void)app;
    (void)conn;
    (void)proto;
    (void)root;
    *out = NULL;
    *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_GATEWAY_POSIX */
