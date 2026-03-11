#include "human/channels/voice_realtime.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_VOICE_RT_ESCAPE_BUF 1024

/* ── SQL escape ───────────────────────────────────────────────────────────── */

static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap)
{
    size_t j = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (s[i] == '\'') {
            if (j + 2 > out_cap)
                return 0;
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            if (j + 1 > out_cap)
                return 0;
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return j;
}

static uint64_t current_time_ms(void)
{
#if defined(HU_IS_TEST) && HU_IS_TEST
    return (uint64_t)time(NULL) * 1000ULL;
#else
    return (uint64_t)time(NULL) * 1000ULL;
#endif
}

/* ── F150-F152: Real-Time Voice Calls ────────────────────────────────────────── */

hu_error_t hu_voice_rt_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS voice_sessions (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    session_id TEXT NOT NULL UNIQUE,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    state TEXT NOT NULL,\n"
        "    codec TEXT NOT NULL,\n"
        "    started_at INTEGER NOT NULL,\n"
        "    ended_at INTEGER,\n"
        "    duration_ms INTEGER NOT NULL,\n"
        "    samples_processed INTEGER NOT NULL\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_voice_rt_insert_sql(const hu_voice_session_t *session, char *buf, size_t cap,
                                  size_t *out_len)
{
    if (!session || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (session->contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char session_esc[HU_VOICE_RT_ESCAPE_BUF];
    char contact_esc[HU_VOICE_RT_ESCAPE_BUF];
    const char *state_str = hu_voice_call_state_str(session->state);
    const char *codec_str = hu_voice_codec_str(session->codec);

    size_t se_len = escape_sql_string(session->session_id, strlen(session->session_id),
                                      session_esc, sizeof(session_esc));
    size_t ce_len = escape_sql_string(session->contact_id, session->contact_id_len, contact_esc,
                                      sizeof(contact_esc));

    if (se_len == 0 && session->session_id[0] != '\0')
        return HU_ERR_INVALID_ARGUMENT;
    if (ce_len == 0 && session->contact_id_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    uint64_t dur_ms = hu_voice_rt_duration_ms(session);
    uint64_t ended_at = (session->state == HU_VOICE_CALL_ENDED ||
                         session->state == HU_VOICE_CALL_FAILED)
                            ? session->started_ms / 1000 + (dur_ms / 1000)
                            : 0;

    int n;
    if (session->state == HU_VOICE_CALL_ENDED || session->state == HU_VOICE_CALL_FAILED) {
        n = snprintf(buf, cap,
                     "INSERT INTO voice_sessions (session_id, contact_id, state, codec, "
                     "started_at, ended_at, duration_ms, samples_processed) "
                     "VALUES ('%s', '%s', '%s', '%s', %llu, %llu, %llu, %llu)",
                     session_esc, contact_esc, state_str, codec_str,
                     (unsigned long long)(session->started_ms / 1000),
                     (unsigned long long)ended_at, (unsigned long long)dur_ms,
                     (unsigned long long)session->samples_processed);
    } else {
        n = snprintf(buf, cap,
                     "INSERT INTO voice_sessions (session_id, contact_id, state, codec, "
                     "started_at, ended_at, duration_ms, samples_processed) "
                     "VALUES ('%s', '%s', '%s', '%s', %llu, NULL, %llu, %llu)",
                     session_esc, contact_esc, state_str, codec_str,
                     (unsigned long long)(session->started_ms / 1000), (unsigned long long)dur_ms,
                     (unsigned long long)session->samples_processed);
    }
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_voice_config_t hu_voice_rt_default_config(void)
{
    hu_voice_config_t c = {0};
    c.sample_rate = 16000;
    c.channels = 1;
    c.codec = HU_VOICE_CODEC_OPUS;
    c.max_duration_ms = 30ULL * 60 * 1000; /* 30min */
    c.silence_timeout_ms = 5000;
    c.vad_enabled = true;
    return c;
}

hu_error_t hu_voice_rt_init_session(const hu_voice_config_t *config, const char *contact_id,
                                    size_t contact_id_len, hu_voice_session_t *session)
{
    if (!config || !session)
        return HU_ERR_INVALID_ARGUMENT;
    if (!contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    memset(session, 0, sizeof(*session));
    size_t copy_len = contact_id_len < sizeof(session->contact_id) - 1
                          ? contact_id_len
                          : sizeof(session->contact_id) - 1;
    memcpy(session->contact_id, contact_id, copy_len);
    session->contact_id[copy_len] = '\0';
    session->contact_id_len = copy_len;

    session->state = HU_VOICE_CALL_IDLE;
    session->codec = config->codec;

    uint64_t ts = (uint64_t)time(NULL);
    int n = snprintf(session->session_id, sizeof(session->session_id), "voice_%.*s_%llu",
                     (int)copy_len, contact_id, (unsigned long long)ts);
    if (n < 0 || (size_t)n >= sizeof(session->session_id))
        return HU_ERR_INVALID_ARGUMENT;

    return HU_OK;
}

bool hu_voice_rt_is_valid_transition(hu_voice_call_state_t from, hu_voice_call_state_t to)
{
    switch (from) {
    case HU_VOICE_CALL_IDLE:
        return to == HU_VOICE_CALL_RINGING || to == HU_VOICE_CALL_FAILED || to == HU_VOICE_CALL_ENDED;
    case HU_VOICE_CALL_RINGING:
        return to == HU_VOICE_CALL_CONNECTED || to == HU_VOICE_CALL_ENDED ||
               to == HU_VOICE_CALL_FAILED;
    case HU_VOICE_CALL_CONNECTED:
        return to == HU_VOICE_CALL_ON_HOLD || to == HU_VOICE_CALL_ENDED || to == HU_VOICE_CALL_FAILED;
    case HU_VOICE_CALL_ON_HOLD:
        return to == HU_VOICE_CALL_CONNECTED || to == HU_VOICE_CALL_ENDED ||
               to == HU_VOICE_CALL_FAILED;
    case HU_VOICE_CALL_ENDED:
    case HU_VOICE_CALL_FAILED:
        return false;
    default:
        return false;
    }
}

hu_error_t hu_voice_rt_transition(hu_voice_session_t *session, hu_voice_call_state_t new_state)
{
    if (!session)
        return HU_ERR_INVALID_ARGUMENT;
    if (!hu_voice_rt_is_valid_transition(session->state, new_state))
        return HU_ERR_INVALID_ARGUMENT;

    if (new_state == HU_VOICE_CALL_ENDED || new_state == HU_VOICE_CALL_FAILED) {
        if (session->state == HU_VOICE_CALL_CONNECTED || session->state == HU_VOICE_CALL_ON_HOLD) {
            session->duration_ms = current_time_ms() - session->started_ms;
        }
    }
    if (new_state == HU_VOICE_CALL_CONNECTED && session->state == HU_VOICE_CALL_RINGING) {
        session->started_ms = current_time_ms();
    }

    session->state = new_state;
    return HU_OK;
}

uint64_t hu_voice_rt_duration_ms(const hu_voice_session_t *session)
{
    if (!session)
        return 0;
    if (session->state == HU_VOICE_CALL_IDLE || session->state == HU_VOICE_CALL_RINGING)
        return 0;
    if (session->state == HU_VOICE_CALL_ENDED || session->state == HU_VOICE_CALL_FAILED)
        return session->duration_ms;
    return current_time_ms() - session->started_ms;
}

hu_error_t hu_voice_rt_build_prompt(hu_allocator_t *alloc, const hu_voice_session_t *session,
                                    char **out, size_t *out_len)
{
    if (!alloc || !session || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *state_str = hu_voice_call_state_str(session->state);
    uint64_t dur_ms = hu_voice_rt_duration_ms(session);
    uint64_t dur_s = dur_ms / 1000;

    size_t cap = 512;
    char *buf = alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int pos = 0;
    if (session->state == HU_VOICE_CALL_CONNECTED || session->state == HU_VOICE_CALL_ON_HOLD) {
        pos = snprintf(buf, cap,
                       "Currently on voice call with %.*s. Duration: %llus. State: %s%s.",
                       (int)session->contact_id_len, session->contact_id,
                       (unsigned long long)dur_s, state_str,
                       session->is_muted ? " (muted)" : "");
    } else if (session->state == HU_VOICE_CALL_RINGING) {
        pos = snprintf(buf, cap, "Voice call ringing with %.*s. State: %s.",
                       (int)session->contact_id_len, session->contact_id, state_str);
    } else if (session->state == HU_VOICE_CALL_IDLE) {
        pos = snprintf(buf, cap, "No active voice call.");

    } else {
        pos = snprintf(buf, cap, "Voice call with %.*s ended. State: %s. Duration: %llus.",
                       (int)session->contact_id_len, session->contact_id, state_str,
                       (unsigned long long)dur_s);
    }

    *out = buf;
    *out_len = (size_t)pos;
    return HU_OK;
}

const char *hu_voice_call_state_str(hu_voice_call_state_t state)
{
    switch (state) {
    case HU_VOICE_CALL_IDLE:
        return "idle";
    case HU_VOICE_CALL_RINGING:
        return "ringing";
    case HU_VOICE_CALL_CONNECTED:
        return "connected";
    case HU_VOICE_CALL_ON_HOLD:
        return "on_hold";
    case HU_VOICE_CALL_ENDED:
        return "ended";
    case HU_VOICE_CALL_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

const char *hu_voice_codec_str(hu_voice_codec_t codec)
{
    switch (codec) {
    case HU_VOICE_CODEC_OPUS:
        return "opus";
    case HU_VOICE_CODEC_PCM16:
        return "pcm16";
    case HU_VOICE_CODEC_AAC:
        return "aac";
    default:
        return "unknown";
    }
}
