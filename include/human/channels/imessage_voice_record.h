#ifndef HU_CHANNELS_IMESSAGE_VOICE_RECORD_H
#define HU_CHANNELS_IMESSAGE_VOICE_RECORD_H

/*
 * Native Messages voice delivery (spec 2026-09-26 W3).
 *
 * Instead of sending a voice reply as a file attachment, h-uman can have
 * Messages record it: the default input is switched to a BlackHole virtual
 * device, Messages' own "Record audio" button is pressed, the Cartesia clip is
 * played into BlackHole, then "Stop" and "Send". The result is a genuine
 * voice memo (chat.db is_audio_message=1, Messages' own Opus encoding).
 *
 * Gated by HU_VOICE_DELIVERY (attachment | shadow | messages), default
 * attachment. This header holds the pure policy; the orchestrator and the
 * macOS port are declared below it as they land.
 *
 * A separate shared header (not imessage.h) so imessage.c can call it without
 * a new cross-file channel include (scripts/check-edge-context-isolation.sh
 * exempts it as iMessage shared infra).
 */

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_VREC_LABEL_RECORD   "Record audio"
#define HU_VREC_LABEL_STOP     "Stop"
#define HU_VREC_LABEL_SEND     "Send"
#define HU_VREC_LABEL_CANCEL   "Cancel audio recording"
#define HU_VREC_BLACKHOLE_NAME "BlackHole 2ch"

typedef enum {
    HU_VOICE_DELIVERY_ATTACHMENT = 0,
    HU_VOICE_DELIVERY_SHADOW,
    HU_VOICE_DELIVERY_MESSAGES,
} hu_voice_delivery_mode_t;

/* NULL or unknown values mean ATTACHMENT (today's behavior). */
hu_voice_delivery_mode_t hu_voice_delivery_mode_parse(const char *s);

/* A voice-memo send: empty text and exactly one local audio file. */
bool hu_voice_record_is_memo_send(size_t message_len, const char *const *media, size_t media_count);

typedef struct {
    bool ax_trusted;
    bool messages_running;
    bool blackhole_present;
    bool real_mic_configured; /* HU_VOICE_REAL_INPUT set and non-empty */
    bool real_mic_present;
    bool real_mic_busy; /* another process is using the real mic */
    double user_idle_sec;
    double min_idle_sec;
} hu_voice_record_facts_t;

typedef enum {
    HU_VREC_OK = 0,
    HU_VREC_NO_AX,
    HU_VREC_NO_MESSAGES,
    HU_VREC_NO_BLACKHOLE,
    HU_VREC_NO_REAL_MIC,
    HU_VREC_MIC_BUSY,
    HU_VREC_USER_ACTIVE,
} hu_voice_record_block_t;

/* Fails closed: NULL facts block as HU_VREC_NO_AX. */
hu_voice_record_block_t hu_voice_record_preflight(const hu_voice_record_facts_t *f);
const char *hu_voice_record_block_name(hu_voice_record_block_t b);

/* Human-sized pauses around the clip: lead-in 350-700 ms, tail 500-900 ms. */
typedef struct {
    uint32_t lead_in_ms;
    uint32_t tail_ms;
} hu_voice_record_timing_t;
void hu_voice_record_timing(uint32_t seed, hu_voice_record_timing_t *out);

typedef enum {
    HU_VREC_ROUTE_ATTACHMENT = 0,
    HU_VREC_ROUTE_SHADOW,
    HU_VREC_ROUTE_RECORD,
} hu_voice_record_route_t;

/* Only memo-shaped sends leave the attachment path, and only when the mode
 * asks for it. */
hu_voice_record_route_t hu_voice_record_route(hu_voice_delivery_mode_t mode, size_t message_len,
                                              const char *const *media, size_t media_count);

#endif /* HU_CHANNELS_IMESSAGE_VOICE_RECORD_H */
