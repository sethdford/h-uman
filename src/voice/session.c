#include "human/voice/session.h"
#include "human/core/log.h"
#include "human/core/time.h"
#include "human/voice/provider.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t voice_session_now_ms(void) {
    return (int64_t)hu_time_wall_ms();
}

static void latency_reset_marks(hu_voice_session_t *session) {
    session->latency_send_mark_ms = 0;
    session->latency_rt_mark_ms = 0;
    session->latency_first_byte_pending = false;
    session->latency_interrupt_mark_ms = 0;
    session->latency_await_interrupt_silence = false;
}

#if !HU_IS_TEST
static void running_avg_update(double *avg, size_t *count, int64_t sample) {
    if (!avg || !count)
        return;
    (*count)++;
    double n = (double)*count;
    double s = (double)sample;
    if (*count <= 1u)
        *avg = s;
    else
        *avg = *avg * ((n - 1.0) / n) + s / n;
}
#endif

hu_error_t hu_voice_session_start(hu_allocator_t *alloc, hu_voice_session_t *session,
                                  const char *channel_name, size_t channel_name_len,
                                  const hu_config_t *config) {
    if (!alloc || !session || !config)
        return HU_ERR_INVALID_ARGUMENT;
    if (channel_name_len > 0 && !channel_name)
        return HU_ERR_INVALID_ARGUMENT;

    memset(session, 0, sizeof(*session));
    hu_duplex_session_init(&session->duplex);

    size_t copy_len = channel_name_len;
    if (copy_len >= sizeof(session->channel_name))
        copy_len = sizeof(session->channel_name) - 1;
    if (channel_name && copy_len > 0)
        memcpy(session->channel_name, channel_name, copy_len);
    session->channel_name[copy_len] = '\0';

#if HU_IS_TEST
    session->started_at = voice_session_now_ms();
    session->active = true;
    return HU_OK;
#else
    session->started_at = voice_session_now_ms();
    session->active = true;

    const char *tp = config->voice.tts_provider;
    const char *rt_model = (config->voice.realtime_model && config->voice.realtime_model[0])
                               ? config->voice.realtime_model
                               : config->voice.tts_model;
    const char *rt_voice = (config->voice.realtime_voice && config->voice.realtime_voice[0])
                               ? config->voice.realtime_voice
                               : config->voice.tts_voice;
    bool realtime_mode = (config->voice.mode && strcmp(config->voice.mode, "realtime") == 0) ||
                         (tp && strcmp(tp, "realtime") == 0);
    if (realtime_mode) {
        const char *key = hu_config_get_provider_key(config, "openai");
        if (key && key[0]) {
            hu_voice_rt_config_t rtc = {.model = rt_model,
                                        .voice = rt_voice,
                                        .api_key = key,
                                        .sample_rate = 24000,
                                        .vad_enabled = true};
            /* Use provider vtable abstraction (preferred path). */
            hu_voice_provider_t vp = {0};
            if (hu_voice_provider_openai_create(alloc, &rtc, &vp) == HU_OK && vp.vtable) {
                if (vp.vtable->connect(vp.ctx) == HU_OK) {
                    session->provider = vp;
                    /* Keep legacy rt pointer for backwards compat with existing callers */
                    session->rt = (hu_voice_rt_session_t *)vp.ctx;
                } else {
                    vp.vtable->disconnect(vp.ctx, alloc);
                }
            }
        }
    }
    return HU_OK;
#endif
}

hu_error_t hu_voice_session_stop(hu_voice_session_t *session) {
    if (!session)
        return HU_ERR_INVALID_ARGUMENT;
    if (session->provider.vtable) {
        session->provider.vtable->disconnect(session->provider.ctx, NULL);
        session->provider.ctx = NULL;
        session->provider.vtable = NULL;
        session->rt = NULL;
    } else if (session->rt) {
        hu_voice_rt_session_destroy(session->rt);
        session->rt = NULL;
    }
    (void)hu_duplex_session_init(&session->duplex);
    session->last_action = HU_TURN_ACTION_NONE;
    session->active = false;
    session->started_at = 0;
    session->last_audio_ms = 0;
    memset(&session->latency, 0, sizeof(session->latency));
    session->latency_avg_round_trip_ms = 0.0;
    session->latency_avg_interrupt_ms = 0.0;
    session->latency_round_trip_measurements = 0;
    session->latency_interrupt_measurements = 0;
    latency_reset_marks(session);
    return HU_OK;
}

hu_error_t hu_voice_session_send_audio(hu_voice_session_t *session, const uint8_t *pcm16,
                                       size_t pcm16_len) {
    if (!session)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->active)
        return HU_ERR_INVALID_ARGUMENT;
    if (pcm16_len > 0 && !pcm16)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t now = voice_session_now_ms();
    session->last_audio_ms = now;

    hu_turn_action_t action = HU_TURN_ACTION_NONE;
    hu_error_t err = hu_duplex_user_chunk(&session->duplex, now, HU_TURN_SIGNAL_NONE, &action);
    session->last_action = action;
    if (err != HU_OK)
        return err;

    if (pcm16_len == 0)
        return HU_OK;

#if HU_IS_TEST
    session->latency.first_byte_ms = 150;
    session->latency.total_round_trip_ms = 400;
    session->latency.interrupt_latency_ms = 50;
    session->latency.measurements = 1;
    session->latency.avg_first_byte_ms = 150.0;
    session->latency_avg_round_trip_ms = 400.0;
    session->latency_avg_interrupt_ms = 50.0;
    session->latency_round_trip_measurements = 1;
    session->latency_interrupt_measurements = 1;
    return HU_OK;
#else
    session->latency_send_mark_ms = now;
    session->latency_rt_mark_ms = now;
    session->latency_first_byte_pending = true;
#endif

    if (session->provider.vtable)
        return session->provider.vtable->send_audio(session->provider.ctx, pcm16, pcm16_len);
    if (session->rt && session->rt->connected)
        return hu_voice_rt_send_audio(session->rt, pcm16, pcm16_len);
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_voice_session_on_interrupt(hu_voice_session_t *session) {
    if (!session)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->active)
        return HU_OK;

    int64_t now = voice_session_now_ms();
    hu_turn_action_t action = HU_TURN_ACTION_NONE;
    (void)hu_duplex_user_chunk(&session->duplex, now, HU_TURN_SIGNAL_INTERRUPT, &action);
    session->last_action = action;

#if !HU_IS_TEST
    session->latency_interrupt_mark_ms = now;
    session->latency_await_interrupt_silence = true;
#endif

    if (session->provider.vtable)
        return session->provider.vtable->cancel_response(session->provider.ctx);
    if (session->rt && session->rt->connected)
        return hu_voice_rt_response_cancel(session->rt);
    return HU_OK;
}

void hu_voice_session_note_response_first_byte(hu_voice_session_t *session) {
    if (!session || !session->active || !session->latency_first_byte_pending)
        return;
#if HU_IS_TEST
    (void)session;
#else
    int64_t now = voice_session_now_ms();
    int64_t dt = now - session->latency_send_mark_ms;
    if (dt < 0)
        dt = 0;
    session->latency.first_byte_ms = dt;
    running_avg_update(&session->latency.avg_first_byte_ms, &session->latency.measurements, dt);
    session->latency_first_byte_pending = false;
#endif
}

void hu_voice_session_note_response_complete(hu_voice_session_t *session) {
    if (!session || !session->active)
        return;
#if HU_IS_TEST
    (void)session;
#else
    int64_t now = voice_session_now_ms();
    int64_t dt = now - session->latency_rt_mark_ms;
    if (dt < 0)
        dt = 0;
    session->latency.total_round_trip_ms = dt;
    running_avg_update(&session->latency_avg_round_trip_ms,
                       &session->latency_round_trip_measurements, dt);
    session->latency_rt_mark_ms = now;
    session->latency_first_byte_pending = false;
#endif
}

void hu_voice_session_note_interrupt_silence(hu_voice_session_t *session) {
    if (!session || !session->active)
        return;
#if HU_IS_TEST
    (void)session;
#else
    if (!session->latency_await_interrupt_silence)
        return;
    int64_t now = voice_session_now_ms();
    int64_t dt = now - session->latency_interrupt_mark_ms;
    if (dt < 0)
        dt = 0;
    session->latency.interrupt_latency_ms = dt;
    running_avg_update(&session->latency_avg_interrupt_ms, &session->latency_interrupt_measurements,
                       dt);
    session->latency_await_interrupt_silence = false;
#endif
}

hu_error_t hu_voice_session_get_latency(const hu_voice_session_t *session,
                                        int64_t *out_avg_first_byte_ms,
                                        int64_t *out_avg_round_trip_ms,
                                        int64_t *out_avg_interrupt_ms) {
    if (!session || !out_avg_first_byte_ms || !out_avg_round_trip_ms || !out_avg_interrupt_ms)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->active)
        return HU_ERR_INVALID_ARGUMENT;
    if (session->latency.measurements == 0 && session->latency_round_trip_measurements == 0 &&
        session->latency_interrupt_measurements == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (session->latency.measurements > 0)
        *out_avg_first_byte_ms = (int64_t)llround(session->latency.avg_first_byte_ms);
    else
        *out_avg_first_byte_ms = 0;

    if (session->latency_round_trip_measurements > 0)
        *out_avg_round_trip_ms = (int64_t)llround(session->latency_avg_round_trip_ms);
    else
        *out_avg_round_trip_ms = 0;

    if (session->latency_interrupt_measurements > 0)
        *out_avg_interrupt_ms = (int64_t)llround(session->latency_avg_interrupt_ms);
    else
        *out_avg_interrupt_ms = 0;

    return HU_OK;
}

void hu_voice_session_warn_first_byte_latency_if_needed(const hu_voice_session_t *session) {
    static int s_warned;
    if (s_warned || !session || !session->active)
        return;
    if (session->latency.measurements == 0)
        return;
    if (session->latency.avg_first_byte_ms <= (double)HU_VOICE_TARGET_FIRST_BYTE_MS)
        return;
    hu_log_error("voice", NULL, "average first-byte latency %.0f ms exceeds target %d ms",
                 session->latency.avg_first_byte_ms, HU_VOICE_TARGET_FIRST_BYTE_MS);
    s_warned = 1;
}

/* ── Micro-turn API ─────────────────────────────────────────────── */

hu_error_t hu_voice_session_user_turn_signal(hu_voice_session_t *session, hu_turn_signal_t signal,
                                             hu_turn_action_t *out_action) {
    if (!session || !out_action)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->active)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t now = voice_session_now_ms();
    hu_error_t err = hu_duplex_user_chunk(&session->duplex, now, signal, out_action);
    session->last_action = *out_action;
    return err;
}

hu_error_t hu_voice_session_agent_turn_signal(hu_voice_session_t *session, hu_turn_signal_t signal,
                                              hu_turn_action_t *out_action) {
    if (!session || !out_action)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->active)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t now = voice_session_now_ms();
    hu_error_t err = hu_duplex_agent_chunk(&session->duplex, now, signal, out_action);
    session->last_action = *out_action;
    return err;
}

hu_turn_action_t hu_voice_session_last_action(const hu_voice_session_t *session) {
    if (!session)
        return HU_TURN_ACTION_NONE;
    return session->last_action;
}

hu_error_t hu_voice_session_recv_event(hu_voice_session_t *session, hu_allocator_t *alloc,
                                       hu_voice_rt_event_t *out, int timeout_ms) {
    if (!session || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->active || !session->provider.vtable || !session->provider.vtable->recv_event)
        return HU_ERR_NOT_SUPPORTED;
    hu_error_t err =
        session->provider.vtable->recv_event(session->provider.ctx, alloc, out, timeout_ms);
    if (err == HU_OK && out->audio_base64 && out->audio_base64_len > 0 &&
        session->latency_first_byte_pending) {
        session->latency.first_byte_ms = voice_session_now_ms() - session->latency_send_mark_ms;
        session->latency_first_byte_pending = false;
        session->latency.measurements++;
        double n = (double)session->latency.measurements;
        session->latency.avg_first_byte_ms = session->latency.avg_first_byte_ms * ((n - 1.0) / n) +
                                             (double)session->latency.first_byte_ms / n;
    }
    return err;
}

hu_error_t hu_voice_session_activity_start(hu_voice_session_t *session) {
    if (!session || !session->active)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->provider.vtable || !session->provider.vtable->send_activity_start)
        return HU_ERR_NOT_SUPPORTED;
    return session->provider.vtable->send_activity_start(session->provider.ctx);
}

hu_error_t hu_voice_session_activity_end(hu_voice_session_t *session) {
    if (!session || !session->active)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->provider.vtable || !session->provider.vtable->send_activity_end)
        return HU_ERR_NOT_SUPPORTED;
    return session->provider.vtable->send_activity_end(session->provider.ctx);
}

hu_error_t hu_voice_session_send_tool_response(hu_voice_session_t *session, const char *name,
                                               const char *call_id, const char *response_json) {
    if (!session || !session->active || !name || !call_id || !response_json)
        return HU_ERR_INVALID_ARGUMENT;
    if (!session->provider.vtable || !session->provider.vtable->send_tool_response)
        return HU_ERR_NOT_SUPPORTED;
    return session->provider.vtable->send_tool_response(session->provider.ctx, name, call_id,
                                                        response_json);
}
