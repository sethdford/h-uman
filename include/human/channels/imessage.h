#ifndef HU_CHANNELS_IMESSAGE_H
#define HU_CHANNELS_IMESSAGE_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_imessage_create(hu_allocator_t *alloc, const char *default_target,
                              size_t default_target_len, const char *const *allow_from,
                              size_t allow_from_count, hu_channel_t *out);
void hu_imessage_destroy(hu_channel_t *ch);

/** Enable the imsg CLI (steipete/imsg) for send/react at runtime.
 * Must be called after hu_imessage_create. */
void hu_imessage_set_use_imsg_cli(hu_channel_t *ch, bool use);

/** Treat is_from_me=1 messages from this handle as incoming (self-test via same Apple ID). */
void hu_imessage_set_loopback_handle(hu_channel_t *ch, const char *handle);

/** Returns true if default target (phone/email) is configured. */
bool hu_imessage_is_configured(hu_channel_t *ch);

/** Returns true if the imsg watch subprocess is actively monitoring for new messages.
 * Event-driven monitoring provides sub-second message delivery latency.
 * Returns false under HU_IS_TEST, on non-macOS, or when imsg CLI is unavailable. */
bool hu_imessage_watch_active(hu_channel_t *ch);

/** Poll ~/Library/Messages/chat.db for new inbound messages (macOS only). */
hu_error_t hu_imessage_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count);

/** Maps hu_reaction_type_t to iMessage tapback name (love, like, dislike, etc.).
 * Returns NULL for HU_REACTION_NONE or unknown. */
const char *hu_imessage_reaction_to_tapback_name(hu_reaction_type_t reaction);

/** Build tapback context string for recent reactions on our messages from this contact.
 * Returns allocated string like "[REACTIONS on your recent messages: 2 hearts, 1 like]" or NULL.
 * Caller owns. Stub returns NULL on non-macOS or when SQLite unavailable. */
hu_error_t hu_imessage_build_tapback_context(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len, char **out, size_t *out_len);

hu_error_t hu_imessage_build_read_receipt_context(hu_allocator_t *alloc, const char *contact_id,
                                                  size_t contact_id_len, char **out,
                                                  size_t *out_len);

/** Count positive tapbacks (love/like/laugh/emphasis) on our GIF messages from this
 * contact in the last 24 hours. Uses direct SQL on chat.db associated_message_type.
 * Returns 0 on non-macOS or when SQLite unavailable. */
int hu_imessage_count_recent_gif_tapbacks(const char *contact_id, size_t contact_id_len);

/** Count positive tapbacks on our music messages (.m4a attachments) to this contact (24h). */
int hu_imessage_count_recent_music_tapbacks(const char *contact_id, size_t contact_id_len);

/** Look up the ROWID of the most recent is_from_me=1 message to the given handle.
 * Used for self-reaction targeting instead of fragile ROWID+1 guessing.
 * Returns -1 on failure or when SQLite/macOS unavailable. */
int64_t hu_imessage_get_latest_sent_rowid(const char *handle, size_t handle_len);

#ifndef HU_IS_TEST
/** Check if the real user sent a message to `handle` within the last
 * `within_seconds` seconds.  Queries chat.db for is_from_me=1 rows.
 * Returns true if the user responded recently (Human should stay silent). */
bool hu_imessage_user_responded_recently(void *channel_ctx, const char *handle, size_t handle_len,
                                         int within_seconds);

/** Query the attachment path for a given message ROWID from chat.db.
 * Returns the attachment file path or NULL if not found. Caller owns. */
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id);

/** Get the attachment path for the most recent message with an attachment from
 * the given contact. Returns path or NULL. Caller owns. */
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len);
#endif

/** Look up the text of a message by its GUID from chat.db.
 * Used for inline reply context — when a message has thread_originator_guid,
 * look up what they're replying to. Writes into out_text buffer. */
hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len);

/** Map an expressive_send_style_id to a human-readable effect name.
 * Returns e.g. "Slam", "Confetti", "Gentle", or NULL if unrecognized.
 * Available on Apple platforms and under HU_IS_TEST. */
const char *hu_imessage_effect_name(const char *style_id);

/** Classify a balloon_bundle_id into a display label.
 * Returns "[Sticker]", "[Memoji]", or "[iMessage App]".
 * Returns NULL if balloon_id is NULL/empty.
 * Available on Apple platforms and under HU_IS_TEST. */
const char *hu_imessage_balloon_label(const char *balloon_id);

/** True if text is NULL, empty, or a COALESCE-generated generic label
 * ("[Photo]", "[Video]", "[Voice Message]") that balloon classification
 * should override. Used by poll, history, and lookup paths. */
bool hu_imessage_text_is_placeholder(const char *text);

/** Copy src into dst with bounded length. Returns bytes written (excluding NUL).
 * Safe when src_len >= dst_cap — truncates and always NUL-terminates. */
size_t hu_imessage_copy_bounded(char *dst, size_t dst_cap, const char *src, size_t src_len);

/** Extract plain text from an NSAttributedString (NSKeyedArchiver) blob.
 * macOS 15+ stores iMessage text in attributedBody instead of the text column.
 * Pure byte parsing — no platform dependencies. Returns extracted length, or 0. */
size_t hu_imessage_extract_attributed_body(const unsigned char *blob, size_t blob_len, char *out,
                                           size_t out_cap);

/** Compute a human-realistic typing duration for a message of the given length.
 * Models real mobile typing: ~45ms/char base rate, 400ms read/compose overhead,
 * seed-driven jitter to prevent statistical detection of linear timing.
 * Returns milliseconds, clamped to [800, 6000]. */
unsigned int hu_imessage_typing_duration(size_t msg_len, uint32_t seed);

/** Search Tenor for a GIF matching the query and download to a temp file.
 * Returns the local path to the downloaded GIF (caller owns, free with alloc).
 * Returns NULL on failure (no API key, network error, no results).
 * Requires HU_ENABLE_CURL. api_key is the Tenor API v2 key. */
char *hu_imessage_fetch_gif(hu_allocator_t *alloc, const char *query, size_t query_len,
                            const char *api_key, size_t api_key_len);

/* ── FDA-aware circuit breaker + poll status ─────────────────────────────
 * The iMessage poller depends on sqlite read access to ~/Library/Messages/chat.db,
 * which macOS Full Disk Access can revoke at any time (notably across rebuilds of
 * unsigned binaries). When that happens, sqlite returns SQLITE_AUTH (23) on every
 * open and the poll/imsg-watch loop will busy-spin forever. The circuit breaker
 * counts consecutive AUTH/CANTOPEN errors and, after HU_IMESSAGE_BREAKER_THRESHOLD,
 * marks the channel unhealthy and stops respawning the imsg watch subprocess.
 * One successful poll resets the breaker. The poll-status file lets the doctor
 * command and external observers see what state the channel is in without
 * tailing logs. */

typedef enum {
    HU_IMESSAGE_ERR_NONE = 0,
    HU_IMESSAGE_ERR_AUTH,     /* SQLITE_AUTH (23) — typically Full Disk Access denied */
    HU_IMESSAGE_ERR_CANTOPEN, /* SQLITE_CANTOPEN (14) — chat.db missing or unreadable */
    HU_IMESSAGE_ERR_BUSY,     /* SQLITE_BUSY/LOCKED — transient, retried internally */
    HU_IMESSAGE_ERR_OTHER,    /* anything else */
} hu_imessage_error_class_t;

/* Pure classifier: maps a sqlite3 return code to an error class. */
hu_imessage_error_class_t hu_imessage_classify_sqlite_error(int rc);

/* Human-readable name for an error class ("NONE", "AUTH", "CANTOPEN", "BUSY",
 * "OTHER"). Always non-NULL. */
const char *hu_imessage_error_class_name(hu_imessage_error_class_t cls);

/* Inverse of hu_imessage_error_class_name. Maps a class name string back to
 * the enum. Returns HU_IMESSAGE_ERR_OTHER for NULL, empty, or unrecognized
 * input. Case-sensitive: matches the exact strings returned by
 * hu_imessage_error_class_name ("NONE", "AUTH", "CANTOPEN", "BUSY",
 * "OTHER"). */
hu_imessage_error_class_t hu_imessage_error_class_from_name(const char *name);

#ifndef HU_IMESSAGE_BREAKER_THRESHOLD
#define HU_IMESSAGE_BREAKER_THRESHOLD 5
#endif

bool hu_imessage_breaker_tripped(const hu_channel_t *ch);
uint32_t hu_imessage_consecutive_failures(const hu_channel_t *ch);
hu_imessage_error_class_t hu_imessage_last_error_class(const hu_channel_t *ch);
int64_t hu_imessage_last_success_epoch(const hu_channel_t *ch);

/* Resolve the poll-status JSON path ("$HOME/.human/imessage.poll_status").
 * Writes into buf; returns false if HOME unset or buffer too small. */
bool hu_imessage_status_path(char *buf, size_t cap);

/* ── Health state machine + watchdog ─────────────────────────────────────
 * The breaker only sees sqlite-level failures. The watchdog also catches the
 * "process is alive but iMessage poll loop has been silently stuck for N
 * seconds" case (e.g. imsg watch hung, lock contention). The state machine
 * collapses both signals into a coarse health enum so the daemon and the
 * doctor can speak a common language. */

typedef enum {
    HU_IMESSAGE_HEALTH_UNKNOWN = 0, /* no successful poll has happened yet */
    HU_IMESSAGE_HEALTH_OK,          /* recent successful poll, breaker not tripped */
    HU_IMESSAGE_HEALTH_STALLED,     /* breaker not tripped but no success in stall_secs */
    HU_IMESSAGE_HEALTH_TRIPPED,     /* breaker tripped (FDA, chat.db missing, etc.) */
} hu_imessage_health_t;

/* Always non-NULL. */
const char *hu_imessage_health_name(hu_imessage_health_t h);

/* Default stall threshold (seconds) used when the daemon does not pass an
 * explicit value. Chosen to be much larger than the poll cadence (1s) so
 * transient backpressure does not produce false STALLED alerts. */
#ifndef HU_IMESSAGE_DEFAULT_STALL_SECS
#define HU_IMESSAGE_DEFAULT_STALL_SECS 120
#endif

/* Pure function: derive current health from the channel's recorded state
 * relative to wall-clock `now_epoch`. Does NOT log or mutate. Safe on NULL. */
hu_imessage_health_t hu_imessage_health(const hu_channel_t *ch, int64_t now_epoch,
                                        int64_t stall_threshold_secs);

/* Periodic watchdog tick. Computes current health and, if it differs from the
 * last logged health, emits a single info/warn line describing the transition
 * and persists the new state to the poll status file. Safe on NULL channels
 * and on channels in HU_IMESSAGE_HEALTH_UNKNOWN. */
void hu_imessage_watchdog_tick(hu_channel_t *ch, int64_t now_epoch, int64_t stall_threshold_secs);

/* Returns the last logged health for this channel, useful for tests asserting
 * that watchdog_tick is edge-triggered (no log spam on repeated calls). */
hu_imessage_health_t hu_imessage_last_logged_health(const hu_channel_t *ch);

#if HU_IS_TEST
/* Drive the breaker accounting from tests with synthetic sqlite return codes.
 * Returns true if this call caused the breaker to trip. */
bool hu_imessage_test_record_open_result(hu_channel_t *ch, int rc, int64_t now_epoch);

/* Drive a synthetic successful poll (resets breaker, advances last_success_epoch). */
void hu_imessage_test_record_poll_success(hu_channel_t *ch, int64_t now_epoch);

/* Mark the imsg-watch process as running (or not) for tests. The pid recorded
 * is purely diagnostic — the test harness must not actually fork imsg. Used
 * to drive `hu_imessage_poll` through its watch-active short-circuit so we
 * can assert the idle-poll heartbeat refreshes the success epoch. */
void hu_imessage_test_set_watch_running(hu_channel_t *ch, bool running);

/* Read the persisted last_successful_poll_epoch (mirrors what the doctor
 * reads from the JSON status file). */
int64_t hu_imessage_test_get_last_success_epoch(const hu_channel_t *ch);
#endif

#if HU_IS_TEST
hu_error_t hu_imessage_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len);
hu_error_t hu_imessage_test_inject_mock_ex(hu_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len, bool has_attachment);
hu_error_t hu_imessage_test_inject_mock_ex2(hu_channel_t *ch, const char *session_key,
                                            size_t session_key_len, const char *content,
                                            size_t content_len, bool has_attachment,
                                            bool has_video);

/** Full-featured mock injection with all message fields. */
typedef struct hu_imessage_test_msg_opts {
    const char *guid;
    const char *reply_to_guid;
    const char *chat_id;
    bool has_attachment;
    bool has_video;
    bool is_group;
    bool was_edited;
    bool was_unsent;
    int64_t timestamp_sec;
} hu_imessage_test_msg_opts_t;

hu_error_t hu_imessage_test_inject_mock_full(hu_channel_t *ch, const char *session_key,
                                             size_t session_key_len, const char *content,
                                             size_t content_len,
                                             const hu_imessage_test_msg_opts_t *opts);

/** Store a GUID→text mapping for lookup_message_by_guid in test builds (per-channel). */
void hu_imessage_test_store_guid_text(hu_channel_t *ch, const char *guid, const char *text);

/** Register a GUID→text entry in the global lookup store.
 * Makes hu_imessage_lookup_message_by_guid return the text for this GUID. */
void hu_imessage_test_set_guid_lookup(const char *guid, const char *text);

/** Clear all registered GUID→text lookup entries. */
void hu_imessage_test_clear_guid_lookups(void);

const char *hu_imessage_test_get_last_message(hu_channel_t *ch, size_t *out_len);
void hu_imessage_test_get_last_reaction(hu_channel_t *ch, hu_reaction_type_t *out_reaction,
                                        int64_t *out_message_id);
size_t hu_imessage_test_get_last_media_count(hu_channel_t *ch);
/** Test hook for Tenor fallback JSON string extraction (`gif_json_extract`). */
size_t hu_imessage_test_gif_json_extract(const char *json, size_t json_len, const char *key,
                                         char *out, size_t cap);

/** US-43.3 test seam: peek at the latest courtesy reply the agent loop drained
 * from the pending-courtesy ring. Returns NULL if nothing has been drained. */
const char *hu_imessage_test_get_last_courtesy_message(hu_channel_t *ch, size_t *out_len);
const char *hu_imessage_test_get_last_courtesy_target(hu_channel_t *ch, size_t *out_len);
#endif

/** US-43.3: enable/disable the courtesy reply path on a live channel.
 * Default is true; the operator opt-out flag in config flows through here. */
void hu_imessage_set_courtesy_replies_enabled(hu_channel_t *ch, bool enabled);

/** US-43.3: drain at most `max_drain` pending courtesy replies, sending each
 * via the channel's send vtable. Called by the agent loop tick, NEVER by the
 * poll thread. Returns the number actually drained (0 if none pending). */
size_t hu_imessage_drain_pending_courtesy(hu_channel_t *ch, size_t max_drain);

/** US-43.3: count of pending courtesy replies and refused-enqueue counter
 * (observability for the operator audit surface). */
size_t hu_imessage_pending_courtesy_count(hu_channel_t *ch);
uint64_t hu_imessage_pending_courtesy_refused(hu_channel_t *ch);

#endif /* HU_CHANNELS_IMESSAGE_H */
