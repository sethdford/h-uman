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
#include <sys/types.h>

#define HU_VREC_LABEL_RECORD   "Record audio"
#define HU_VREC_LABEL_STOP     "Stop"
#define HU_VREC_LABEL_SEND     "Send"
#define HU_VREC_LABEL_CANCEL   "Cancel audio recording"
#define HU_VREC_BLACKHOLE_NAME "BlackHole 2ch"
/* Messages finalizes the recording after Stop; a Send pressed sooner is
 * silently ignored (live test 2026-09-27). Distinct from the lead-in/tail
 * ranges (<= 900 ms). */
#define HU_VREC_SEND_SETTLE_MS 1000u

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
    bool real_mic_busy;             /* another process is using the real mic */
    bool default_input_is_real_mic; /* else a call may be on another mic (AirPods) */
    bool blackhole_busy;            /* another app already routes audio into BlackHole */
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
    HU_VREC_INPUT_NOT_REAL_MIC,
    HU_VREC_BLACKHOLE_BUSY,
    HU_VREC_BAD_HANDLE,   /* not a plain phone number or email: never open it */
    HU_VREC_LOCKED,       /* another recording is in progress (any process) */
    HU_VREC_UNKNOWN_CHAT, /* Messages could not name the target conversation */
} hu_voice_record_block_t;

/* Fails closed: NULL facts block as HU_VREC_NO_AX. */
hu_voice_record_block_t hu_voice_record_preflight(const hu_voice_record_facts_t *f);
const char *hu_voice_record_block_name(hu_voice_record_block_t b);

/* Only plain phone numbers (+digits, >= 7 digits) and emails are opened —
 * never group GUIDs or anything carrying URL syntax. */
bool hu_voice_record_handle_ok(const char *handle, size_t handle_len);

/* The open conversation is the target: Messages' window title equals the
 * display name Messages itself gives the target handle (trimmed, case-folded).
 * Empty on either side never matches — the wrong-recipient check fails closed. */
bool hu_voice_record_title_matches(const char *window_title, const char *expected_title);

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
    hu_error_t (*chat_title)(void *ctx, char *buf, size_t cap); /* open conversation's title */
    hu_error_t (*expected_title)(void *ctx, const char *handle, size_t handle_len, char *buf,
                                 size_t cap); /* Messages' own display name for the handle */
    double (*idle_sec)(void *ctx);            /* seconds since the last keyboard/mouse input */
    uint64_t (*now_ms)(void *ctx);            /* monotonic */
    bool (*try_lock)(void *ctx);              /* one recording at a time, across processes */
    void (*unlock)(void *ctx);
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
    bool cancel_failed;            /* a recording may be left in the compose bar */
    const char *abort_reason;      /* static: wrong_chat, user_returned, send_unconfirmed, ... */
    int64_t prior_max_rowid;
} hu_voice_record_result_t;

/* HU_OK: the memo left (see out->verified; never re-send on !verified).
 * HU_ERR_NOT_SUPPORTED: blocked before anything was touched.
 * HU_ERR_IO: failed before Send; recording cancelled and input/UI restored —
 * the caller falls back to the attachment send. */
hu_error_t hu_voice_record_send(const hu_voice_record_port_t *port,
                                const hu_voice_record_request_t *req,
                                hu_voice_record_result_t *out);

/* Request from the environment: HU_VOICE_REAL_INPUT (the mic to restore;
 * "" when unset, which preflight blocks as no_real_mic) and
 * HU_VOICE_MIN_IDLE_SEC (default 20). Shared by imessage_send and the CLI. */
void hu_voice_record_request_from_env(const char *handle, size_t handle_len, const char *audio_path,
                                      uint32_t seed, hu_voice_record_request_t *out);

/* Build the request from the environment and run it on the macOS port. Same
 * return contract as hu_voice_record_send. */
hu_error_t hu_voice_record_send_from_env(const char *handle, size_t handle_len,
                                         const char *audio_path, hu_voice_record_result_t *out);

/* ── macOS port (src/channels/imessage_voice_record_macos.c) ─────────────
 * Apple production builds: CoreAudio + AudioQueue + AX + IOKit. Test and
 * non-Apple builds: a stub whose preflight always blocks (HU_VREC_NO_AX) and
 * whose members are all safe no-ops, so the symbol links everywhere. */
const hu_voice_record_port_t *hu_voice_record_macos_port(void);

/* chat.db helpers, defined in imessage.c (which owns the sqlite access) for
 * Apple production builds only. -1 / false when unavailable. */
int64_t hu_imessage_chatdb_max_rowid(void);
pid_t hu_imessage_messages_pid(void); /* 0 when Messages is not running */
bool hu_imessage_chatdb_audio_from_me_after(const char *handle, size_t handle_len,
                                            int64_t after_rowid);

#endif /* HU_CHANNELS_IMESSAGE_VOICE_RECORD_H */
