#ifndef HU_IMESSAGE_INTERNAL_H
#define HU_IMESSAGE_INTERNAL_H

/*
 * Cross-module signatures and shared state for the carved-out iMessage modules.
 *
 * NOT part of the public API at include/human/channels/imessage.h. Consumers
 * of iMessage from outside src/channels/ continue to use only the public
 * header. This file is the internal contract between the imessage*.c files.
 *
 * Layout:
 *   - Shared constants (sent-ring sizes, status-file paths).
 *   - Shared `hu_imessage_ctx_t` struct definition.
 *   - Cross-module function declarations grouped by source module.
 *
 * Visibility note: until the build system gains a `-fvisibility=hidden`
 * default for libhuman_core, these symbols are still externally linkable.
 * The contract is by header inclusion + naming convention — treat any
 * symbol declared here as INTERNAL. Do not call from outside
 * src/channels/imessage*.c.
 */

#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <sys/types.h> /* pid_t */
#endif

#ifdef HU_ENABLE_SQLITE
struct sqlite3;
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define HU_IMESSAGE_SENT_RING_SIZE  32
#define HU_IMESSAGE_SENT_PREFIX_LEN 256
#define HU_IMESSAGE_ROWID_FILE      ".human/imessage.rowid"
#define HU_IMESSAGE_STATUS_FILE     ".human/imessage.poll_status"

/* ── Shared ctx struct ───────────────────────────────────────────────────
 *
 * Owned by imessage.c (the channel factory creates and frees it). Carved-out
 * modules read/write fields directly. Each field is documented in-place
 * where its bookkeeping function lives. */

typedef struct hu_imessage_ctx {
    hu_allocator_t *alloc;
    char *default_target;
    size_t default_target_len;
    bool running;
    int64_t last_rowid;
    const char *const *allow_from;
    size_t allow_from_count;
    char sent_ring[HU_IMESSAGE_SENT_RING_SIZE][HU_IMESSAGE_SENT_PREFIX_LEN];
    size_t sent_ring_len[HU_IMESSAGE_SENT_RING_SIZE];
    uint32_t sent_ring_hash[HU_IMESSAGE_SENT_RING_SIZE];
    size_t sent_ring_idx;
    char typing_last_target[128];
    size_t typing_last_target_len;
    _Atomic bool typing_active;
    bool use_imsg_cli;
    bool imsg_cli_checked;
    bool has_imsg_cli;
    const char *loopback_handle;
    int64_t last_ai_send_epoch;
    /* FDA-aware circuit breaker + poll status (always present so tests + non-Apple
     * builds can interrogate state without #ifdef gymnastics). */
    uint32_t consecutive_open_failures;
    bool circuit_breaker_tripped;
    bool breaker_log_emitted; /* one-shot log gate */
    hu_imessage_error_class_t last_error_class;
    int64_t last_successful_poll_epoch;
    /* Watchdog state machine — collapses breaker-tripped + poll-stalled into a
     * single coarse health enum so the daemon emits exactly ONE log line per
     * transition rather than spamming on every tick. */
    hu_imessage_health_t last_logged_health;
    /* When the breaker is tripped, the poller short-circuits to HU_OK / 0
     * messages on most ticks but probes chat.db once every N ticks so FDA
     * recovery is detected promptly. The counter resets on each probe and
     * after a successful poll. */
    uint32_t breaker_recovery_probe_counter;
    /* `imsg_watch_running` is always compiled so test builds can drive the
     * watch-active code path through the test seam (the prod fields below
     * remain platform-gated). */
    bool imsg_watch_running;
    /* C6: cache chat.db schema column detection. -1 = unchecked, 0 = absent,
     * 1 = present. Set lazily on first poll; chat.db schema doesn't change
     * at runtime so the value is stable for the daemon's lifetime. Avoids
     * a redundant `SELECT date_retracted FROM message LIMIT 0` prepare on
     * every poll tick. */
    int has_date_retracted_cached;
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
    pid_t imsg_watch_pid;
    int imsg_watch_fd;
    bool imsg_target_validated;
    void *imcore_handle;
    bool imcore_tried;
    bool imcore_connected;
#endif
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    size_t last_media_count;
    char last_media_path[256];
    struct {
        char session_key[128];
        char content[4096];
        char guid[96];
        char reply_to_guid[96];
        char chat_id[128];
        bool has_attachment;
        bool has_video;
        bool is_group;
        bool was_edited;
        bool was_unsent;
        int64_t timestamp_sec;
    } mock_msgs[8];
    size_t mock_count;
    char mock_guid_store[8][96];
    size_t mock_guid_count;
    hu_reaction_type_t last_reaction;
    int64_t last_reaction_message_id;
#endif
} hu_imessage_ctx_t;

/* ── Shared helpers (imessage.c) ───────────────────────────────────────── */

/* Sent-ring tracking (used by send + poll to suppress echo of our own sends). */
uint32_t imessage_hash(const char *s, size_t len);
void imessage_record_sent(hu_imessage_ctx_t *c, const char *msg, size_t msg_len);
bool imessage_was_sent_by_us(hu_imessage_ctx_t *c, const char *text, size_t text_len);

/* Poll-status bookkeeping (used by poll + start/stop + the watchdog). */
void imessage_save_poll_status(const hu_imessage_ctx_t *c);
bool imessage_record_open_result(hu_imessage_ctx_t *c, int rc, int64_t now);
void imessage_record_poll_success(hu_imessage_ctx_t *c, int64_t now);
void imessage_record_poll_heartbeat(hu_imessage_ctx_t *c, int64_t now);

/* Chat.db opening (used by poll + react + lookups). */
#ifdef HU_ENABLE_SQLITE
int imessage_open_chatdb(const char *db_path, struct sqlite3 **db_out);
#endif

/* Output sanitizer (used by send and any other module that emits text to Messages). */
size_t imessage_sanitize_output(char *buf, size_t len);

/* AX→tapback action prefix mapping (used by react module). */
const char *imessage_reaction_to_ax_action_prefix(hu_reaction_type_t reaction);

/* ── imsg CLI helpers (imessage_watch.c after Step 3) ─────────────────── */

/* ── Watch module — src/channels/imessage_watch.c (Step 3) ────────────
 *
 * imsg CLI lifecycle: availability probe, watch subprocess (start/stop/
 * has-data), target validation, and the CLI-based react fallback used
 * by the tapback dispatcher. Apple-only; non-Apple builds skip the
 * subprocess entirely and the watch flag stays false. */

bool imsg_cli_available(hu_imessage_ctx_t *c);

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
void imsg_watch_start(hu_imessage_ctx_t *c);
void imsg_watch_stop(hu_imessage_ctx_t *c);
bool imsg_watch_has_data(hu_imessage_ctx_t *c);
bool imsg_validate_target(hu_imessage_ctx_t *c);
bool imsg_try_react(hu_imessage_ctx_t *c, int64_t message_id, hu_reaction_type_t reaction);
#endif

/* AppleScript escape helper (used by typing + send). */
size_t escape_for_applescript(char *out, size_t out_cap, const char *in, size_t in_len);

/* ── Typing module — src/channels/imessage_typing.c (Step 2) ───────────
 *
 * Vtable hooks (always defined; non-Apple / test builds return early).
 * The Tier-3 AppleScript simulator is exposed because the send path
 * triggers it directly when the daemon hasn't pre-activated typing. */

hu_error_t imessage_start_typing(void *ctx, const char *recipient, size_t recipient_len);
hu_error_t imessage_stop_typing(void *ctx, const char *recipient, size_t recipient_len);

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
void imessage_simulate_typing(hu_imessage_ctx_t *c, const char *tgt, size_t tgt_len,
                              size_t message_len);
#endif

/* ── Send module — src/channels/imessage_send.c (Step 6) ──────────────
 *
 * Text + media send vtable hook, plus the AppleScript attachment-script
 * builder (kept in send.c because send is its only caller). */

hu_error_t imessage_send(void *ctx, const char *target, size_t target_len, const char *message,
                         size_t message_len, const char *const *media, size_t media_count);

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
size_t imessage_build_attach_script(char *out, size_t out_cap, const char *target_escaped,
                                    const char *path_escaped);
#endif

/* ── Poll module — src/channels/imessage_poll.c (Step 5) ──────────────
 *
 * chat.db reads: history, attachment paths, GUID lookup, the main poll
 * function, and the user-activity probe. Functions are extracted one at
 * a time so each carries its own behavior preservation evidence.
 *
 * imessage_load_conversation_history is the vtable hook (extern so the
 * channel vtable in imessage.c can point at it). */

hu_error_t imessage_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                              const char *contact_id, size_t contact_id_len,
                                              size_t limit, hu_channel_history_entry_t **out,
                                              size_t *out_count);

/* Rowid persistence (~/.human/imessage.rowid). Used by poll.c to checkpoint
 * the last-seen chat.db ROWID across daemon restarts. */
void imessage_rowid_path(char *buf, size_t cap);
int64_t imessage_load_rowid(void);
void imessage_save_rowid(int64_t rowid);

/* ── React module — src/channels/imessage_react.c (Step 4) ────────────
 *
 * Vtable hook for tapback reactions. 3-tier fallback (imsg CLI → AX →
 * JXA). Test-mode + non-Apple builds short-circuit; HU_IMESSAGE_TAPBACK_
 * ENABLED gates Tiers 2-3. */

hu_error_t imessage_react(void *ctx, const char *target, size_t target_len, int64_t message_id,
                          hu_reaction_type_t reaction);

/* ── AX module — src/channels/imessage_ax.c (Step 1) ──────────────────── */

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
void ax_open_conversation(const char *recipient, size_t recipient_len);
bool ax_start_typing(const char *target, size_t target_len);
bool ax_stop_typing(void);
#ifdef HU_IMESSAGE_TAPBACK_ENABLED
bool ax_tapback(const char *content_prefix, int row_offset, const char *tapback_label);
#endif
#endif

#endif /* HU_IMESSAGE_INTERNAL_H */
