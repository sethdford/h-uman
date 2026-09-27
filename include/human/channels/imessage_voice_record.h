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

/* ── Orchestrator ────────────────────────────────────────────────────────
 * Every real-world effect goes through this port; tests supply a fake. */
typedef struct hu_voice_record_port {
    void *ctx;
    hu_error_t (*gather_facts)(void *ctx, const char *real_mic, hu_voice_record_facts_t *out);
    hu_error_t (*set_input)(void *ctx, const char *device_name); /* sets default input */
    hu_error_t (*get_input)(void *ctx, char *buf, size_t cap);   /* default input's name */
    hu_error_t (*remember_ui)(void *ctx);
    void (*restore_ui)(void *ctx);
    hu_error_t (*open_chat)(void *ctx, const char *handle, size_t handle_len);
    hu_error_t (*press)(void *ctx, const char *label);
    bool (*wait_label)(void *ctx, const char *label, uint32_t timeout_ms);
    hu_error_t (*playback_prepare)(void *ctx, const char *audio_path);
    hu_error_t (*playback_run)(void *ctx); /* blocks until the clip has fully played */
    void (*playback_dispose)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);
    int64_t (*max_rowid)(void *ctx); /* -1 unknown */
    bool (*audio_row_after)(void *ctx, const char *handle, size_t handle_len, int64_t after_rowid,
                            uint32_t timeout_ms);
} hu_voice_record_port_t;

typedef struct {
    const char *handle;
    size_t handle_len;
    const char *audio_path;
    const char *real_mic; /* HU_VOICE_REAL_INPUT: the input to restore, always */
    double min_idle_sec;
    uint32_t seed;
} hu_voice_record_request_t;

typedef enum {
    HU_VREC_STAGE_NONE = 0,
    HU_VREC_STAGE_PREFLIGHT,
    HU_VREC_STAGE_INPUT,
    HU_VREC_STAGE_OPEN,
    HU_VREC_STAGE_RECORD,
    HU_VREC_STAGE_PLAY,
    HU_VREC_STAGE_STOP,
    HU_VREC_STAGE_SENT,
} hu_voice_record_stage_t;

typedef struct {
    hu_voice_record_block_t block;
    hu_voice_record_stage_t stage; /* furthest stage reached */
    bool verified;                 /* chat.db shows the memo */
    bool restored;                 /* default input read back == real_mic */
    int64_t prior_max_rowid;
} hu_voice_record_result_t;

/* HU_OK: Send was pressed (see out->verified; never re-send on !verified).
 * HU_ERR_NOT_SUPPORTED: preflight blocked, nothing touched.
 * HU_ERR_IO: failed before Send; recording cancelled and input/UI restored —
 * the caller falls back to the attachment send. */
hu_error_t hu_voice_record_send(const hu_voice_record_port_t *port,
                                const hu_voice_record_request_t *req,
                                hu_voice_record_result_t *out);

#endif /* HU_CHANNELS_IMESSAGE_VOICE_RECORD_H */
