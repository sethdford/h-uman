#ifndef HU_CHANNELS_VOICE_REALTIME_H
#define HU_CHANNELS_VOICE_REALTIME_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── F150-F152: Real-Time Voice Calls ───────────────────────────────────────
 * Stub/framework API for future WebRTC/voice synthesis integration.
 * No external I/O in production paths; real-time requires external infra.
 * ───────────────────────────────────────────────────────────────────────── */

typedef enum hu_voice_call_state {
    HU_VOICE_CALL_IDLE = 0,
    HU_VOICE_CALL_RINGING,
    HU_VOICE_CALL_CONNECTED,
    HU_VOICE_CALL_ON_HOLD,
    HU_VOICE_CALL_ENDED,
    HU_VOICE_CALL_FAILED
} hu_voice_call_state_t;

typedef enum hu_voice_codec {
    HU_VOICE_CODEC_OPUS = 0,
    HU_VOICE_CODEC_PCM16,
    HU_VOICE_CODEC_AAC
} hu_voice_codec_t;

typedef struct hu_voice_session {
    char session_id[64];
    char contact_id[128];
    size_t contact_id_len;
    hu_voice_call_state_t state;
    hu_voice_codec_t codec;
    uint64_t started_ms;
    uint64_t duration_ms;
    uint64_t samples_processed;
    bool is_muted;
} hu_voice_session_t;

typedef struct hu_voice_config {
    uint32_t sample_rate;       /* default 16000 */
    uint8_t channels;           /* default 1 */
    hu_voice_codec_t codec;
    uint64_t max_duration_ms;
    uint32_t silence_timeout_ms; /* default 5000 */
    bool vad_enabled;           /* default true */
} hu_voice_config_t;

/* SQL schema for voice_sessions table */
hu_error_t hu_voice_rt_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* SQL for recording a session row */
hu_error_t hu_voice_rt_insert_sql(const hu_voice_session_t *session, char *buf, size_t cap,
                                  size_t *out_len);

/* Default config: 16kHz, mono, OPUS, 30min max, 5s silence timeout, VAD on */
hu_voice_config_t hu_voice_rt_default_config(void);

/* Initialize session: generate session_id, copy contact_id, set state to IDLE */
hu_error_t hu_voice_rt_init_session(const hu_voice_config_t *config, const char *contact_id,
                                    size_t contact_id_len, hu_voice_session_t *session);

/* Validate and apply state transition */
hu_error_t hu_voice_rt_transition(hu_voice_session_t *session, hu_voice_call_state_t new_state);

/* Check if transition from -> to is valid */
bool hu_voice_rt_is_valid_transition(hu_voice_call_state_t from, hu_voice_call_state_t to);

/* Current duration: ended ? duration_ms : (now - started_ms); IDLE => 0 */
uint64_t hu_voice_rt_duration_ms(const hu_voice_session_t *session);

/* Build prompt context for active call (alloc-owned output) */
hu_error_t hu_voice_rt_build_prompt(hu_allocator_t *alloc, const hu_voice_session_t *session,
                                    char **out, size_t *out_len);

const char *hu_voice_call_state_str(hu_voice_call_state_t state);
const char *hu_voice_codec_str(hu_voice_codec_t codec);

#endif /* HU_CHANNELS_VOICE_REALTIME_H */
