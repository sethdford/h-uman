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

/* See hu_telegram_set_persona for the contract. Channel name is "imessage". */
struct hu_persona;
void hu_imessage_set_persona(hu_channel_t *ch, const struct hu_persona *persona);

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

/* Map an emoji to the native tapback kind it corresponds to. Never returns
 * HU_REACTION_NONE — an unmapped emoji falls back to THUMBS_UP, because a
 * NONE would re-open the plain-text fallback that shipped emoji as MESSAGES
 * (live 2026-07-20: "tapback emoji sent"=0 vs "flat fallback"=29). */
hu_reaction_type_t hu_imessage_reaction_for_emoji(const char *emoji_utf8);

/* Mark a contact's thread as read (native IMCore read receipt, `imsg read`).
 * A human who replies has, by definition, read the thread — before this the
 * daemon replied while leaving messages permanently "unread" on the sender's
 * side, which is an obvious tell. Gated on HU_IMSG_VERB_READ_RECEIPT; a no-op
 * returning HU_ERR_NOT_SUPPORTED when the bridge is down. Best-effort: never
 * blocks or fails a send. */
hu_error_t hu_imessage_mark_read(void *ctx, const char *target, size_t target_len);

/** Build tapback context string for recent reactions on our messages from this contact.
 * Returns allocated string like "[REACTIONS on your recent messages: 2 hearts, 1 like]" or NULL.
 * Caller owns. Stub returns NULL on non-macOS or when SQLite unavailable. */
hu_error_t hu_imessage_build_tapback_context(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len, char **out, size_t *out_len);

hu_error_t hu_imessage_build_read_receipt_context(hu_allocator_t *alloc, const char *contact_id,
                                                  size_t contact_id_len, char **out,
                                                  size_t *out_len);

/** Find the most recent outbound message to `contact_id` that was READ
 * (date_read > 0) but has NO inbound reply since. Sets *out_msg_id to
 * the chat.db ROWID and *out_read_at_ms to the wall-clock ms of the
 * read receipt; sets both to 0 when there is no unreplied read.
 *
 * Returns HU_OK whether or not a result is found (no result is HU_OK
 * with *out_msg_id == 0). Returns non-OK only on I/O / SQL failures.
 *
 * In test mode (HU_IS_TEST) returns HU_OK with no result — chat.db
 * fixtures are not yet set up; the daemon glue's call site is reviewed,
 * not unit-tested here. The follow-up policy predicates that consume
 * this query's output ARE fully tested in tests/test_follow_up.c. */
hu_error_t hu_imessage_find_unreplied_read(const char *contact_id, size_t contact_id_len,
                                           int64_t *out_msg_id, uint64_t *out_read_at_ms);

/** Find the most recent INBOUND message from `contact_id` that has NO
 * outbound REPLY from seth since. Sets *out_msg_id to the chat.db ROWID
 * and *out_read_at_ms to the wall-clock ms of the inbound message; sets
 * both to 0 when there is no unreplied inbound (user has responded or
 * no inbound exists).
 *
 * Returns HU_OK whether or not a result is found. Returns non-OK only on
 * I/O / SQL failures.
 *
 * This is the inverse of hu_imessage_find_unreplied_read: it detects when
 * the USER (seth) is being unresponsive to a contact's message, enabling
 * proactive follow-ups via the daemon follow-up watcher (US-48-3).
 *
 * In test mode (HU_IS_TEST) returns HU_OK with no result. */
hu_error_t hu_imessage_find_inbound_unreplied(const char *contact_id, size_t contact_id_len,
                                              int64_t *out_msg_id, uint64_t *out_read_at_ms);

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

/** Walk a batch of inbound channel messages; if any carries reply_to_guid,
 * look up the original via hu_imessage_lookup_message_by_guid and format
 * via hu_conversation_build_inline_reply_hint into out_buf.
 *
 * Returns bytes written to out_buf (excluding any trailing NUL). Returns 0
 * when the batch has no reply, the lookup misses, or out_cap is too small.
 * The hint string itself does NOT include a trailing newline; callers that
 * concatenate multiple hints append their own separator.
 *
 * Pure-predicate extraction of the daemon's inline-reply context-building
 * branch so the end-to-end composition (batch → lookup → hint) is unit-testable
 * via hu_imessage_test_set_guid_lookup without driving the full daemon loop. */
size_t hu_imessage_build_inline_reply_hint_for_batch(hu_allocator_t *alloc,
                                                     const hu_channel_loop_msg_t *msgs,
                                                     size_t msg_count, char *out_buf,
                                                     size_t out_cap);

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

/* ── IMCore selector-conformance check ───────────────────────────────────────
 * h-uman's native iMessage path binds a handful of PRIVATE IMCore selectors via
 * the ObjC runtime. When Apple renames or re-signatures one on an OS bump, the
 * bind silently returns nil or a hardcoded zero — see the 2026-07-21 typing
 * phantom, where -[IMChat isCurrentlyTyping] and -[IMAccount loggedIn] had been
 * removed on macOS 26 yet were still called, reporting 0 forever. A boot-time
 * conformance pass turns that silent drift into one loud log line.
 *
 * Factored as a pure predicate + injected resolver so it is unit-testable
 * without the ObjC runtime (which is compiled out under HU_IS_TEST). */
typedef struct hu_imcore_selector_req {
    const char *class_name; /* e.g. "IMChat" */
    const char *selector;   /* e.g. "setLocalUserIsTyping:" */
    bool is_class_method;   /* true for +class methods (e.g. "sharedInstance") */
} hu_imcore_selector_req_t;

/** The canonical table of IMCore selectors h-uman's iMessage path depends on.
 * Sets *count to the entry count. Never returns NULL. */
const hu_imcore_selector_req_t *hu_imessage_imcore_required_selectors(size_t *count);

/** Resolver: does class_name respond to selector? is_class_method selects
 * +class vs -instance lookup. ud is opaque caller data. Must be non-NULL. */
typedef bool (*hu_imcore_selector_resolver_fn)(const char *class_name, const char *selector,
                                               bool is_class_method, void *ud);
/** Reporter: invoked once per selector that does NOT resolve. May be NULL. */
typedef void (*hu_imcore_selector_missing_fn)(const char *class_name, const char *selector,
                                              bool is_class_method, void *ud);

/** Pure conformance pass: for each of the n reqs, call resolve(); for each that
 * returns false, call on_missing() (when non-NULL). Returns the count of
 * unresolved selectors (0 == fully conformant). Returns 0 if reqs or resolve is
 * NULL. No ObjC dependency — the caller injects the resolver. */
size_t hu_imessage_imcore_conformance(const hu_imcore_selector_req_t *reqs, size_t n,
                                      hu_imcore_selector_resolver_fn resolve,
                                      hu_imcore_selector_missing_fn on_missing, void *ud);

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

/** Detect the user's own iMessage handle from ~/Library/Messages/chat.db.
 *
 * Reads the Messages.app SQLite database and queries for the "Me" record
 * (the logged-in user's handle). Returns the handle string (e.g. "+15551234567"
 * or "user@icloud.com") in buf, or HU_ERR_NOT_FOUND if not available.
 *
 * Returns:
 *   HU_OK — buf filled with NUL-terminated handle (< buf_size)
 *   HU_ERR_NOT_FOUND — chat.db unreadable or "Me" record missing
 *   HU_ERR_IO — database I/O error
 *   HU_ERR_INVALID_ARGUMENT — buf or buf_size invalid
 *
 * Non-macOS: returns HU_ERR_NOT_SUPPORTED.
 * Under HU_IS_TEST: returns HU_ERR_NOT_FOUND (fixture-based testing preferred). */
hu_error_t hu_imessage_detect_self_handle(hu_allocator_t *alloc, char *buf, size_t buf_size);

/* ── Non-allowlisted sender courtesy reply (US-9.3) ─────────────────────
 *
 * When a DM arrives from a handle not in the channel's allow_from list, the
 * historical behavior was to silently `continue` past the message. US-9.3
 * replaces that with a rate-limited one-time-per-handle-per-24h courtesy
 * reply explaining the allowlist requirement. The decision is exposed as a
 * pure predicate per `.claude/rules/security-predicate-extraction.md` so
 * tests can pin every row of the truth table without spawning a daemon or
 * writing to chat.db.
 *
 * Aggregate spoof-spam mitigation: even if many spoofed handles spray the
 * daemon, no more than HU_IMESSAGE_COURTESY_DAILY_CAP courtesy replies are
 * emitted in any single 24h bucket (per `aggregate_today_count` input).
 */

#ifndef HU_IMESSAGE_COURTESY_DAILY_CAP
#define HU_IMESSAGE_COURTESY_DAILY_CAP 50
#endif

/* Pure predicate: should we send a courtesy reply right now?
 *
 * Inputs (all booleans + one count):
 *   allowlist_has_handle   — handle IS in allow_from → false (caller never asks for allowlisted).
 *   dedup_already_replied  — same (handle, bucket) already received a reply → false.
 *   courtesy_replies_enabled — operator config flag, default true.
 *   aggregate_today_count  — number of courtesy replies already sent in this 24h bucket
 *                            across ALL handles. If ≥ HU_IMESSAGE_COURTESY_DAILY_CAP → false.
 *
 * Returns true ONLY when: the handle is NOT allowlisted AND we have not
 * already replied to this handle in this bucket AND the feature is enabled
 * AND we have not blown the aggregate spoof-spam cap. Pure: no I/O, no
 * logging, no mutation.
 */
bool hu_imessage_should_courtesy_reply(bool allowlist_has_handle, bool dedup_already_replied,
                                       bool courtesy_replies_enabled,
                                       uint32_t aggregate_today_count);

/* Pure text builder: format a courtesy reply into a caller-provided buffer.
 *
 *   persona_name        — display name to identify the assistant ("Atlas").
 *                         NULL → generic "the human assistant".
 *   owner_display_name  — operator's preferred display name ("Jane"). NULL → "the operator".
 *
 * Security invariants enforced by builder + tests:
 *   • The output NEVER contains a literal phone number or e-mail handle
 *     ("+1..." or "@..."): only display names are interpolated.
 *   • The output MUST contain the substring "allowlist".
 *   • Safe with NULL persona_name and NULL owner_display_name.
 *
 * Writes a NUL-terminated string. Returns the number of bytes written
 * (excluding the terminator). Returns 0 if out_cap < 32 (defensive).
 */
size_t hu_imessage_build_courtesy_reply(const char *persona_name, const char *owner_display_name,
                                        char *out, size_t out_cap);

/* Resolve the courtesy dedup-log path ("$HOME/.human/imessage_courtesy.log").
 * Writes into buf; returns false if HOME unset or buffer too small. */
bool hu_imessage_courtesy_log_path(char *buf, size_t cap);

/* Check whether the (handle, bucket) pair has already received a courtesy
 * reply. Reads the dedup log; bucket = floor(epoch / 86400). Returns false
 * on any I/O failure (fail-open for sends, fail-safe for replies — we
 * prefer "send anyway" to "silent drop forever after a transient FS hiccup").
 */
bool hu_imessage_courtesy_dedup_check(const char *handle, int64_t bucket);

/* Record (handle, bucket) in the dedup log AND increment the aggregate
 * per-bucket counter. Best-effort; silently no-ops on I/O failure (we'd
 * rather double-reply than crash the daemon). Truncates the file when it
 * grows beyond HU_IMESSAGE_COURTESY_LOG_MAX_LINES entries (keep tail). */
void hu_imessage_courtesy_dedup_record(const char *handle, int64_t bucket);

/* Returns the count of courtesy replies recorded in the dedup log for the
 * given bucket. Used by the predicate's aggregate-cap input. Returns 0 if
 * the log is missing or unreadable. */
uint32_t hu_imessage_courtesy_aggregate_count(int64_t bucket);

#ifndef HU_IMESSAGE_COURTESY_LOG_MAX_LINES
#define HU_IMESSAGE_COURTESY_LOG_MAX_LINES 256
#endif

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

/* Test-only accessor for the courtesy-reply mirror field. After the poll
 * loop emits a courtesy reply, the channel ctx records the same text here
 * so tests can assert without confusing concurrent agent-driven sends. */
const char *hu_imessage_test_get_last_courtesy_message(hu_channel_t *ch, size_t *out_len);

/* Test-only: drive the courtesy-reply pipeline as if a non-allowlisted DM
 * had been observed by the poller. Computes bucket from `mock_epoch`,
 * checks the dedup log, applies the predicate, sends via imessage_send,
 * and records to the dedup log + last_courtesy_message mirror.
 *
 * Returns HU_OK on success regardless of whether a reply was sent. The
 * test asserts the *observable* side-effects (last_courtesy_message set
 * or unchanged) — never the rc.
 */
hu_error_t hu_imessage_test_handle_non_allowlisted(hu_channel_t *ch, const char *handle,
                                                   int64_t mock_epoch);

/* Test-only: clear the courtesy-reply mirror (for setup between assertions). */
void hu_imessage_test_clear_last_courtesy_message(hu_channel_t *ch);

/* Test-only: directly invoke the chat.db BUSY-exhaustion handler so tests
 * can pin AC-9.3.3 without spawning a real chat.db lock contention. */
void hu_imessage_test_record_chatdb_busy_exhaustion(hu_channel_t *ch);

/* Test-only: reset the one-shot BUSY log gate (simulating a successful poll). */
void hu_imessage_test_reset_chatdb_busy_gate(hu_channel_t *ch);

/* Test-only: query whether the one-shot BUSY warn was emitted. */
bool hu_imessage_test_chatdb_busy_log_emitted(hu_channel_t *ch);
void hu_imessage_test_get_last_reaction(hu_channel_t *ch, hu_reaction_type_t *out_reaction,
                                        int64_t *out_message_id);
size_t hu_imessage_test_get_last_media_count(hu_channel_t *ch);
/** Test hook for Tenor fallback JSON string extraction (`gif_json_extract`). */
size_t hu_imessage_test_gif_json_extract(const char *json, size_t json_len, const char *key,
                                         char *out, size_t cap);

/** Test-only: set a callback function pointer that will be invoked instead of osascript send.
 * Callback receives (target, target_len, message, message_len) and should log the send
 * attempt to a file or buffer for test assertions. Pass NULL to disable the stub. */
typedef void (*hu_imessage_test_send_stub_fn)(const char *target, size_t target_len,
                                              const char *message, size_t message_len);
void hu_imessage_set_test_send_stub(hu_imessage_test_send_stub_fn fn);

/** Test-only — replaces sub-picker AX with a deterministic stub.
 * Pass NULL to disable the stub and revert to the real AX path (if available). */
void hu_imessage_set_test_react_emoji_stub(bool (*stub)(const char *emoji_utf8));

/** Test-only: deterministic check of CLASSIC_MAP lookup + fallback. */
const char *hu_imessage_test_classic_label_for_emoji(const char *emoji_utf8);
#endif

/** Public: attempt to react with an arbitrary emoji via AX sub-picker.
 * Returns HU_OK on success, HU_ERR_NOT_SUPPORTED if not available
 * (caller can then fall back to classic-tapback mapping in D2).
 * Returns HU_ERR_INVALID_ARGUMENT if emoji_utf8 is NULL or empty. */
hu_error_t hu_imessage_react_emoji_subpicker(void *ctx, const char *target, size_t target_len,
                                             int64_t message_id, const char *emoji_utf8);

/** Public dispatcher: try sub-picker first; on miss, fall back to
 * CLASSIC_MAP nearest-classic-tapback. Never fails entirely — map-miss
 * defaults to "Liked" per Seth's universal-positive choice.
 * Signature matches vtable->react_emoji slot exactly. */
hu_error_t hu_imessage_react_emoji_with_fallback(void *ctx, const char *target, size_t target_len,
                                                 int64_t message_id, const char *emoji_utf8,
                                                 size_t emoji_utf8_len);

/** Capability probe for native iMessage sticker send.
 * macOS exposes NO automation API to send a native sticker/Memoji balloon, so
 * the imessage vtable sets .send_sticker = NULL and this function is unreachable
 * in production (it always returns HU_ERR_NOT_SUPPORTED there). Retained as a
 * tested probe; see docs/investigations/imessage-sticker-memoji-feasibility.md.
 * To send an expressive image, use the regular attachment path (vtable->send).
 * sticker_path is an absolute filesystem path to the sticker image file.
 * Returns HU_OK only via a test stub; HU_ERR_INVALID_ARGUMENT for null/empty
 * args; HU_ERR_NOT_FOUND if the file doesn't exist; HU_ERR_NOT_SUPPORTED in
 * production. Signature matches the vtable->send_sticker slot exactly. */
hu_error_t hu_imessage_send_sticker(void *ctx, const char *target, size_t target_len,
                                    const char *sticker_path, size_t sticker_path_len);

/** Threaded-reply parent matching (BUG #3). Returns true iff `prefix` occurs in
 * `haystack` beginning at a WORD BOUNDARY (string start or after a
 * non-alphanumeric byte). The trailing edge is unconstrained because the
 * iMessage parent prefix is truncated mid-word, so a both-sided word-boundary
 * match would spuriously fail. Used by the macOS AX parent-bubble lookup to
 * avoid resolving the wrong parent on a mid-token substring hit. Pure and
 * NULL-safe; defined unconditionally so it is unit-testable in every build. */
bool hu_imessage_desc_prefix_match(const char *haystack, const char *prefix);

#ifdef HU_IS_TEST
/** Test-only: set a callback function pointer that will be invoked instead of imsg send.
 * Callback receives (target, target_len, sticker_path) and should return HU_OK on success
 * or HU_ERR_NOT_SUPPORTED on failure. Pass NULL to disable the stub and use production
 * behavior. */
void hu_imessage_set_test_send_sticker_stub(hu_error_t (*stub)(const char *target,
                                                               size_t target_len,
                                                               const char *sticker_path));
#endif

#endif /* HU_CHANNELS_IMESSAGE_H */
