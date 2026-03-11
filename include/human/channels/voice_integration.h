#ifndef HU_CHANNELS_VOICE_INTEGRATION_H
#define HU_CHANNELS_VOICE_INTEGRATION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── F147-F149: Incoming Voice Understanding ───────────────────────────────── */

typedef struct hu_voice_analysis {
    char *transcription;
    size_t transcription_len;
    char *detected_emotion;
    size_t detected_emotion_len; /* "happy", "stressed", "calm", etc. */
    double emotion_confidence;
    double speech_rate; /* words per minute */
    bool contains_laughter;
    bool contains_sighing;
    uint32_t duration_seconds;
} hu_voice_analysis_t;

hu_error_t hu_voice_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_voice_insert_analysis_sql(const hu_voice_analysis_t *a, const char *contact_id,
                                         size_t cid_len, char *buf, size_t cap, size_t *out_len);
hu_error_t hu_voice_build_prompt(hu_allocator_t *alloc, const hu_voice_analysis_t *a, char **out,
                                  size_t *out_len);

/* Detect emotion from transcription text (keyword-based fallback) */
hu_error_t hu_voice_detect_emotion_text(const char *text, size_t text_len, char *emotion_out,
                                         size_t emotion_cap, double *confidence);

void hu_voice_analysis_deinit(hu_allocator_t *alloc, hu_voice_analysis_t *a);

/* ── F169-F173: FaceTime/Phone Call Integration ────────────────────────────── */

typedef enum hu_call_event_type {
    HU_CALL_INCOMING = 0,
    HU_CALL_OUTGOING,
    HU_CALL_MISSED,
    HU_CALL_DECLINED,
    HU_CALL_FACETIME_INCOMING,
    HU_CALL_FACETIME_MISSED
} hu_call_event_type_t;

typedef struct hu_call_event {
    hu_call_event_type_t type;
    char *contact_id;
    size_t contact_id_len;
    uint64_t timestamp;
    uint32_t duration_seconds; /* 0 for missed/declined */
} hu_call_event_t;

typedef struct hu_call_response_config {
    bool auto_text_on_missed;   /* default true */
    bool auto_text_on_declined; /* default true */
    uint32_t response_delay_seconds; /* default 30 */
} hu_call_response_config_t;

hu_error_t hu_call_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_call_insert_event_sql(const hu_call_event_t *e, char *buf, size_t cap,
                                    size_t *out_len);
hu_error_t hu_call_query_recent_sql(const char *contact_id, size_t len, uint32_t limit, char *buf,
                                    size_t cap, size_t *out_len);

/* Should we auto-text after this call event? */
bool hu_call_should_auto_text(const hu_call_event_t *e,
                               const hu_call_response_config_t *config);

/* Build text response prompt for missed/declined calls */
hu_error_t hu_call_build_text_prompt(hu_allocator_t *alloc, hu_call_event_type_t type,
                                     const char *contact_name, size_t name_len, char **out,
                                     size_t *out_len);

const char *hu_call_event_type_str(hu_call_event_type_t t);
void hu_call_event_deinit(hu_allocator_t *alloc, hu_call_event_t *e);

#endif /* HU_CHANNELS_VOICE_INTEGRATION_H */
