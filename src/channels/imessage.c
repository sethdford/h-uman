#include "human/channels/imessage.h"
#include "human/channel_loop.h"
#include "human/context/conversation.h"
#include "human/core/error.h"
#include "human/core/io_secure.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "imessage_internal.h" /* cross-module signatures for carved-out iMessage modules */
#ifndef HU_CODENAME
#define HU_CODENAME "human"
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <sys/wait.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif
#endif

/* Shared constants + ctx struct now live in imessage_internal.h. */

size_t hu_imessage_extract_attributed_body(const unsigned char *blob, size_t blob_len, char *out,
                                           size_t out_cap) {
    if (!blob || blob_len < 4 || !out || out_cap < 2)
        return 0;

    for (size_t i = 0; i + 3 < blob_len; i++) {
        if (blob[i] == 0x01 && blob[i + 1] == 0x2B) {
            size_t text_len = 0;
            size_t text_start = 0;
            unsigned char lb = blob[i + 2];
            if (lb < 0x80) {
                text_len = lb;
                text_start = i + 3;
            } else {
                size_t len_bytes = lb & 0x7F;
                if (len_bytes == 0 || len_bytes > 4 || i + 3 + len_bytes > blob_len)
                    return 0;
                for (size_t b = 0; b < len_bytes; b++)
                    text_len |= (size_t)blob[i + 3 + b] << (8 * b);
                text_start = i + 3 + len_bytes;
            }

            if (text_start + text_len > blob_len)
                text_len = blob_len - text_start;
            if (text_len >= out_cap)
                text_len = out_cap - 1;
            memcpy(out, blob + text_start, text_len);
            out[text_len] = '\0';
            return text_len;
        }
    }
    return 0;
}

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
static void imessage_rowid_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (home)
        snprintf(buf, cap, "%s/" HU_IMESSAGE_ROWID_FILE, home);
    else
        buf[0] = '\0';
}

static int64_t imessage_load_rowid(void) {
    char path[512];
    imessage_rowid_path(path, sizeof(path));
    if (!path[0])
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int64_t rowid = 0;
    if (fscanf(f, "%lld", (long long *)&rowid) != 1)
        rowid = 0;
    fclose(f);
    return rowid;
}

static void imessage_save_rowid(int64_t rowid) {
    char path[512];
    imessage_rowid_path(path, sizeof(path));
    if (!path[0])
        return;
    /* iMessage cursor (last-seen rowid) at ~/.human/imessage_rowid.
     * No secret content; 0644. Path derived from $HOME. */
    FILE *f = NULL;
    if (hu_io_secure_open(path, HU_IO_PERM_USER, "w", &f) != HU_OK || !f)
        return;
    fprintf(f, "%lld\n", (long long)rowid);
    fclose(f);
}

#endif

/* ── FDA-aware circuit breaker: pure / no-struct helpers ───────────────── */

/* sqlite3 result codes are stable ABI (see SQLITE_OK/AUTH/etc. in sqlite3.h).
 * We use literal values here so the classifier compiles in any build profile,
 * including test builds that do not include sqlite3.h. */
hu_imessage_error_class_t hu_imessage_classify_sqlite_error(int rc) {
    switch (rc) {
    case 0:
        return HU_IMESSAGE_ERR_NONE; /* SQLITE_OK */
    case 23:
        return HU_IMESSAGE_ERR_AUTH; /* SQLITE_AUTH */
    case 14:
        return HU_IMESSAGE_ERR_CANTOPEN; /* SQLITE_CANTOPEN */
    case 5:                              /* SQLITE_BUSY */
    case 6:
        return HU_IMESSAGE_ERR_BUSY; /* SQLITE_LOCKED */
    default:
        return HU_IMESSAGE_ERR_OTHER;
    }
}

const char *hu_imessage_error_class_name(hu_imessage_error_class_t cls) {
    switch (cls) {
    case HU_IMESSAGE_ERR_NONE:
        return "NONE";
    case HU_IMESSAGE_ERR_AUTH:
        return "AUTH";
    case HU_IMESSAGE_ERR_CANTOPEN:
        return "CANTOPEN";
    case HU_IMESSAGE_ERR_BUSY:
        return "BUSY";
    case HU_IMESSAGE_ERR_OTHER:
        return "OTHER";
    }
    return "OTHER";
}

bool hu_imessage_status_path(char *buf, size_t cap) {
    if (!buf || cap < 16)
        return false;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    int n = snprintf(buf, cap, "%s/" HU_IMESSAGE_STATUS_FILE, home);
    return n > 0 && (size_t)n < cap;
}

/* ── Circuit breaker / status: ctx-dependent helpers (always compiled) ── */

/* Persist the channel's poll status to disk. Best-effort: silently no-ops if
 * HOME is unset or the file cannot be written. The format is intentionally a
 * tiny, hand-emitted JSON so it can be inspected with `cat` and parsed by the
 * doctor command without pulling in a JSON library at this layer. Creates
 * $HOME/.human if it does not exist (matches the rowid file's lifetime
 * assumption). */
void imessage_save_poll_status(const hu_imessage_ctx_t *c) {
    if (!c)
        return;
    char path[512];
    if (!hu_imessage_status_path(path, sizeof(path)))
        return;
    /* Ensure ~/.human exists. mkdir is idempotent (EEXIST ignored). Also
     * mkdir the parent so test fixtures that pin HOME to a fresh tmp path
     * don't silently fail before .human is reached. */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        (void)mkdir(home, 0700);
        char dir[512];
        int dn = snprintf(dir, sizeof(dir), "%s/.human", home);
        if (dn > 0 && (size_t)dn < sizeof(dir))
            (void)mkdir(dir, 0700);
    }
    /* iMessage channel config at ~/.human/imessage.json. Contains
     * allow-list contacts (PII) and channel preferences; 0600 to
     * match the daemon's other config files. */
    FILE *f = NULL;
    if (hu_io_secure_open(path, HU_IO_PERM_SECRET, "w", &f) != HU_OK || !f)
        return;
    fprintf(f,
            "{\n"
            "  \"last_rowid\": %lld,\n"
            "  \"last_successful_poll_epoch\": %lld,\n"
            "  \"consecutive_open_failures\": %u,\n"
            "  \"circuit_breaker_tripped\": %s,\n"
            "  \"last_error_class\": \"%s\"\n"
            "}\n",
            (long long)c->last_rowid, (long long)c->last_successful_poll_epoch,
            (unsigned)c->consecutive_open_failures, c->circuit_breaker_tripped ? "true" : "false",
            hu_imessage_error_class_name(c->last_error_class));
    fclose(f);
}

/* Core breaker accounting. Pure with respect to time (caller passes `now`).
 * Returns true if this call caused the breaker to trip on this invocation. */
bool imessage_record_open_result(hu_imessage_ctx_t *c, int rc, int64_t now) {
    if (!c)
        return false;
    hu_imessage_error_class_t cls = hu_imessage_classify_sqlite_error(rc);
    c->last_error_class = cls;
    if (cls == HU_IMESSAGE_ERR_NONE) {
        bool was_tripped = c->circuit_breaker_tripped;
        c->consecutive_open_failures = 0;
        c->circuit_breaker_tripped = false;
        c->breaker_log_emitted = false;
        c->last_successful_poll_epoch = now;
        if (was_tripped)
            hu_log_info("imessage", NULL, "circuit breaker reset (chat.db open succeeded)");
        return false;
    }
    if (cls == HU_IMESSAGE_ERR_BUSY || cls == HU_IMESSAGE_ERR_OTHER) {
        /* Transient or unrelated failures do NOT trip the breaker; they would
         * mask real BUSY backoff and obscure novel errors that need attention. */
        return false;
    }
    /* AUTH or CANTOPEN: consecutive count drives the breaker. */
    if (c->consecutive_open_failures < UINT32_MAX)
        c->consecutive_open_failures++;
    bool just_tripped = false;
    if (!c->circuit_breaker_tripped &&
        c->consecutive_open_failures >= HU_IMESSAGE_BREAKER_THRESHOLD) {
        c->circuit_breaker_tripped = true;
        just_tripped = true;
    }
    if (just_tripped && !c->breaker_log_emitted) {
        c->breaker_log_emitted = true;
        hu_log_error("imessage", NULL,
                     "circuit breaker tripped after %u consecutive %s errors — likely Full "
                     "Disk Access revoked. Run `human doctor imessage` and re-grant FDA on "
                     "the binary at $(readlink -f $(which human)).",
                     (unsigned)c->consecutive_open_failures, hu_imessage_error_class_name(cls));
    }
    return just_tripped;
}

void imessage_record_poll_success(hu_imessage_ctx_t *c, int64_t now) {
    (void)imessage_record_open_result(c, 0, now);
}

/* Record a heartbeat from a healthy idle watch tick.
 *
 * Production failure (2026-05-10): `human doctor imessage` reported STALE
 * within ~120s of every daemon restart even when `imsg watch` was alive
 * and the channel was perfectly healthy. The first poll after watch
 * startup updated `last_successful_poll_epoch`, then the value froze
 * because the watch-active short-circuit returned HU_OK without recording
 * any subsequent heartbeat. This helper centralizes the policy: every
 * healthy idle poll IS a heartbeat. The disk write is ~170 bytes, well
 * within the daemon's 1Hz cadence budget. */
/* Marked unused at the language level because both callsites are inside
 * compile-time guards (#if HU_IS_TEST and the macOS+SQLite production
 * branch). On Linux or no-SQLite builds neither caller is compiled in,
 * which would otherwise trip -Werror=unused-function. */
void imessage_record_poll_heartbeat(hu_imessage_ctx_t *c, int64_t now) __attribute__((unused));
void imessage_record_poll_heartbeat(hu_imessage_ctx_t *c, int64_t now) {
    if (!c)
        return;
    imessage_record_poll_success(c, now);
    imessage_save_poll_status(c);
}

bool hu_imessage_breaker_tripped(const hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    return ((const hu_imessage_ctx_t *)ch->ctx)->circuit_breaker_tripped;
}

uint32_t hu_imessage_consecutive_failures(const hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return 0;
    return ((const hu_imessage_ctx_t *)ch->ctx)->consecutive_open_failures;
}

hu_imessage_error_class_t hu_imessage_last_error_class(const hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return HU_IMESSAGE_ERR_NONE;
    return ((const hu_imessage_ctx_t *)ch->ctx)->last_error_class;
}

int64_t hu_imessage_last_success_epoch(const hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return 0;
    return ((const hu_imessage_ctx_t *)ch->ctx)->last_successful_poll_epoch;
}

/* ── Health state + watchdog (always compiled) ─────────────────────────── */

const char *hu_imessage_health_name(hu_imessage_health_t h) {
    switch (h) {
    case HU_IMESSAGE_HEALTH_OK:
        return "OK";
    case HU_IMESSAGE_HEALTH_STALLED:
        return "STALLED";
    case HU_IMESSAGE_HEALTH_TRIPPED:
        return "TRIPPED";
    case HU_IMESSAGE_HEALTH_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

hu_imessage_health_t hu_imessage_health(const hu_channel_t *ch, int64_t now_epoch,
                                        int64_t stall_threshold_secs) {
    if (!ch || !ch->ctx)
        return HU_IMESSAGE_HEALTH_UNKNOWN;
    const hu_imessage_ctx_t *c = (const hu_imessage_ctx_t *)ch->ctx;
    if (c->circuit_breaker_tripped)
        return HU_IMESSAGE_HEALTH_TRIPPED;
    if (c->last_successful_poll_epoch <= 0)
        return HU_IMESSAGE_HEALTH_UNKNOWN;
    if (stall_threshold_secs <= 0)
        stall_threshold_secs = HU_IMESSAGE_DEFAULT_STALL_SECS;
    /* Guard against clock skew: if `now` predates the recorded success, treat
     * as OK rather than spuriously STALLED. */
    if (now_epoch < c->last_successful_poll_epoch)
        return HU_IMESSAGE_HEALTH_OK;
    if (now_epoch - c->last_successful_poll_epoch > stall_threshold_secs)
        return HU_IMESSAGE_HEALTH_STALLED;
    return HU_IMESSAGE_HEALTH_OK;
}

hu_imessage_health_t hu_imessage_last_logged_health(const hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return HU_IMESSAGE_HEALTH_UNKNOWN;
    return ((const hu_imessage_ctx_t *)ch->ctx)->last_logged_health;
}

void hu_imessage_watchdog_tick(hu_channel_t *ch, int64_t now_epoch, int64_t stall_threshold_secs) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    hu_imessage_health_t cur = hu_imessage_health(ch, now_epoch, stall_threshold_secs);
    /* Don't emit anything until the channel has produced its first signal —
     * UNKNOWN is "we don't know yet", not a transition to log. */
    if (cur == HU_IMESSAGE_HEALTH_UNKNOWN)
        return;
    if (cur == c->last_logged_health)
        return;

    hu_imessage_health_t prev = c->last_logged_health;
    int64_t since_success =
        c->last_successful_poll_epoch > 0 ? (now_epoch - c->last_successful_poll_epoch) : -1;

    /* Single edge-triggered line per transition. Recovery is info, degradation
     * is warn, breaker-trip is left to imessage_record_open_result which
     * already emits an error with FDA remediation guidance — we only mention
     * the transition target so log readers can correlate. */
    switch (cur) {
    case HU_IMESSAGE_HEALTH_OK:
        hu_log_info("imessage", NULL, "iMessage health %s → OK (recovered; last success %llds ago)",
                    hu_imessage_health_name(prev), (long long)since_success);
        break;
    case HU_IMESSAGE_HEALTH_STALLED:
        hu_log_warn("imessage", NULL,
                    "iMessage health %s → STALLED (no successful poll for %llds, threshold=%llds; "
                    "imsg watch may be hung — try `launchctl kickstart -k gui/$UID/"
                    "ai.human.service-loop`)",
                    hu_imessage_health_name(prev), (long long)since_success,
                    (long long)stall_threshold_secs);
        break;
    case HU_IMESSAGE_HEALTH_TRIPPED:
        hu_log_warn("imessage", NULL, "iMessage health %s → TRIPPED (see breaker error above)",
                    hu_imessage_health_name(prev));
        break;
    case HU_IMESSAGE_HEALTH_UNKNOWN:
    default:
        break;
    }
    c->last_logged_health = cur;
    /* Persist transition so external observers (doctor, monitoring) see it
     * without needing to tail logs. */
    imessage_save_poll_status(c);
}

#if HU_IS_TEST
bool hu_imessage_test_record_open_result(hu_channel_t *ch, int rc, int64_t now_epoch) {
    if (!ch || !ch->ctx)
        return false;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    bool tripped = imessage_record_open_result(c, rc, now_epoch);
    imessage_save_poll_status(c);
    return tripped;
}

void hu_imessage_test_record_poll_success(hu_channel_t *ch, int64_t now_epoch) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    imessage_record_poll_success(c, now_epoch);
    imessage_save_poll_status(c);
}

void hu_imessage_test_set_watch_running(hu_channel_t *ch, bool running) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    c->imsg_watch_running = running;
}

int64_t hu_imessage_test_get_last_success_epoch(const hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return 0;
    return ((const hu_imessage_ctx_t *)ch->ctx)->last_successful_poll_epoch;
}
#endif

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)

/* AX (Accessibility) bridge for typing + tapback lives in
 * src/channels/imessage_ax.c. Cross-module signatures
 * (ax_open_conversation / ax_start_typing / ax_stop_typing / ax_tapback)
 * are declared in src/channels/imessage_internal.h.
 *
 * IMCore (private framework, Tier-1 typing) is forward-declared here for now;
 * it'll move with imessage_typing.c in Step 2 of the refactor. */

uint32_t imessage_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

void imessage_record_sent(hu_imessage_ctx_t *c, const char *msg, size_t msg_len) {
    c->last_ai_send_epoch = (int64_t)time(NULL);
    size_t slot = c->sent_ring_idx % HU_IMESSAGE_SENT_RING_SIZE;
    size_t copy_len =
        msg_len < HU_IMESSAGE_SENT_PREFIX_LEN - 1 ? msg_len : HU_IMESSAGE_SENT_PREFIX_LEN - 1;
    memcpy(c->sent_ring[slot], msg, copy_len);
    c->sent_ring[slot][copy_len] = '\0';
    c->sent_ring_len[slot] = copy_len;
    c->sent_ring_hash[slot] = imessage_hash(msg, msg_len);
    c->sent_ring_idx++;
}

#ifdef HU_ENABLE_SQLITE
bool imessage_was_sent_by_us(hu_imessage_ctx_t *c, const char *text, size_t text_len) {
    uint32_t h = imessage_hash(text, text_len);
    for (size_t i = 0; i < HU_IMESSAGE_SENT_RING_SIZE; i++) {
        size_t slen = c->sent_ring_len[i];
        if (slen == 0)
            continue;
        if (c->sent_ring_hash[i] == h) {
            size_t cmp_len = text_len < slen ? text_len : slen;
            if (cmp_len > 0 && memcmp(text, c->sent_ring[i], cmp_len) == 0)
                return true;
        }
    }
    return false;
}
#endif

#ifdef HU_ENABLE_SQLITE
/** Open chat.db readonly with a 3s busy timeout to tolerate Messages.app locks.
 * Retries up to 3 times with exponential backoff (100ms, 200ms, 400ms)
 * when the database is locked. */
int imessage_open_chatdb(const char *db_path, sqlite3 **db_out) {
    int rc = SQLITE_OK;
    for (int attempt = 0; attempt < 3; attempt++) {
        *db_out = NULL;
        rc = sqlite3_open_v2(db_path, db_out, SQLITE_OPEN_READONLY, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_busy_timeout(*db_out, 3000);
            return SQLITE_OK;
        }
        if (*db_out) {
            sqlite3_close(*db_out);
            *db_out = NULL;
        }
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
            return rc;
        hu_log_info("imessage", NULL, "chat.db locked (attempt %d/3, rc=%d), retrying", attempt + 1,
                    rc);
        usleep((unsigned)(100000 << attempt));
    }
    return rc;
}

bool hu_imessage_user_responded_recently(void *channel_ctx, const char *handle, size_t handle_len,
                                         int within_seconds) {
    if (!handle || handle_len == 0 || within_seconds <= 0)
        return false;

    const char *home = getenv("HOME");
    if (!home)
        return false;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return false;

    /*
     * Check for is_from_me=1 messages to this handle within the time window.
     * macOS Messages stores dates as nanoseconds since 2001-01-01 (Core Data epoch).
     */
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;
    if (c && c->loopback_handle && handle_len > 0 && strlen(c->loopback_handle) == handle_len &&
        memcmp(c->loopback_handle, handle, handle_len) == 0)
        return false;

    /* Exclude messages sent by the AI: only count is_from_me=1 rows whose
     * timestamp is after the last known AI send (+3s grace for clock skew).
     * If the AI never sent, last_ai_send_epoch is 0 and the filter is a no-op. */
    int64_t ai_cutoff = 0;
    if (c && c->last_ai_send_epoch > 0)
        ai_cutoff = c->last_ai_send_epoch + 3;

    const char *sql = "SELECT COUNT(*) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "AND m.date > ((?2 - 978307200) * 1000000000) "
                      "AND m.date > ((?3 - 978307200) * 1000000000)";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, handle, (int)handle_len, NULL);

    time_t cutoff = time(NULL) - within_seconds;
    sqlite3_bind_int64(stmt, 2, (int64_t)cutoff);
    sqlite3_bind_int64(stmt, 3, ai_cutoff);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = sqlite3_column_int(stmt, 0) > 0;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}
#endif
#endif

static hu_error_t imessage_start(void *ctx) {
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
    if (c->use_imsg_cli && imsg_cli_available(c)) {
        imsg_validate_target(c);
        imsg_watch_start(c);
    }
#endif
    return HU_OK;
}

static void imessage_stop(void *ctx) {
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (c) {
        c->running = false;
        atomic_store(&c->typing_active, false);
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
        imsg_watch_stop(c);
        if (c->imcore_handle) {
            dlclose(c->imcore_handle);
            c->imcore_handle = NULL;
            c->imcore_connected = false;
        }
#endif
    }
}

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST

/* Safety net: delegates to the canonical AI phrase stripper in conversation.c,
 * then collapses double spaces and trims whitespace. Modifies in-place. */
size_t imessage_sanitize_output(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;

    len = hu_conversation_strip_ai_phrases(buf, len);

    /* Collapse double spaces */
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == ' ' && buf[i - 1] == ' ') {
            memmove(buf + i, buf + i + 1, len - i);
            len--;
            i--;
        }
    }

    /* Trim leading whitespace */
    while (len > 0 && (buf[0] == ' ' || buf[0] == '\t')) {
        memmove(buf, buf + 1, len);
        len--;
    }

    /* Trim trailing whitespace */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }

    return len;
}

/* Pure lookup / classification helpers (reaction names, effect names,
 * balloon labels, placeholder detection, bounded copy) live in
 * src/channels/imessage_classify.c. This is the first carve-out from the
 * 4400-LOC imessage.c — see docs/plans/2026-05-12-imessage-shape-refactor.md
 * for the full sequence. The public API in include/human/channels/imessage.h
 * is unchanged. */

#if defined(HU_IMESSAGE_TAPBACK_ENABLED)
/*
 * Tapback mapping: hu_reaction_type_t -> iMessage AX context menu label.
 * chat.db associated_message_type values (for reference when reading tapbacks):
 *   2000=love, 2001=like, 2002=dislike, 2003=laugh, 2004=emphasis, 2005=question.
 * Sending uses JXA + System Events AXShowMenu; AppleScript has no native tapback API.
 * Requires accessibility permissions; UI hierarchy may vary by macOS version.
 */
const char *imessage_reaction_to_ax_action_prefix(hu_reaction_type_t reaction) {
    /* macOS 26 SwiftUI Messages: tapbacks are AX actions on the message
     * element named "Name:Heart", "Name:Thumbs up", etc. */
    switch (reaction) {
    case HU_REACTION_HEART:
        return "Name:Heart";
    case HU_REACTION_THUMBS_UP:
        return "Name:Thumbs up";
    case HU_REACTION_THUMBS_DOWN:
        return "Name:Thumbs down";
    case HU_REACTION_HAHA:
        return "Name:Ha ha!";
    case HU_REACTION_EMPHASIS:
        return "Name:Exclamation mark";
    case HU_REACTION_QUESTION:
        return "Name:Question mark";
    case HU_REACTION_CUSTOM_EMOJI:
        return NULL;
    default:
        return NULL;
    }
}
#endif

/* Escape for AppleScript string literal: quotes, backslash, and control chars.
 * Control characters (0x00-0x1F, 0x7F) are stripped to prevent script injection. */
size_t escape_for_applescript(char *out, size_t out_cap, const char *in, size_t in_len) {
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 2 < out_cap; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch < 0x20 || ch == 0x7F) {
            continue;
        } else if (in[i] == '\\' || in[i] == '"') {
            out[j++] = '\\';
            out[j++] = in[i];
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return j;
}

unsigned int hu_imessage_typing_duration(size_t msg_len, uint32_t seed) {
    uint32_t s = seed * 1103515245u + 12345u;
    int32_t jitter = (int32_t)((s >> 16u) % 801u) - 300;
    unsigned int base = 400u + (unsigned int)(msg_len * 45u) + (unsigned int)jitter;
    if (base < 800u)
        base = 800u;
    if (base > 6000u)
        base = 6000u;
    return base;
}

#endif

static const char *imessage_name(void *ctx) {
    (void)ctx;
    return "imessage";
}
static bool imessage_health_check(void *ctx) {
#if !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    return false;
#elif HU_IS_TEST
    /* Honor a tripped breaker even in tests so unit tests can exercise the
     * unhealthy path; otherwise tests stay healthy by default. */
    if (ctx) {
        const hu_imessage_ctx_t *c = (const hu_imessage_ctx_t *)ctx;
        if (c->circuit_breaker_tripped)
            return false;
    }
    return true;
#else
    if (ctx) {
        const hu_imessage_ctx_t *c = (const hu_imessage_ctx_t *)ctx;
        if (c->circuit_breaker_tripped)
            return false;
    }
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;
    if (access(db_path, R_OK) != 0) {
        fprintf(
            stderr,
            "[%s] imessage: ~/Library/Messages/chat.db not readable (Full Disk Access required)\n",
            HU_CODENAME);
        return false;
    }
    return true;
#endif
}

static hu_error_t imessage_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                     const char *contact_id, size_t contact_id_len,
                                                     size_t limit, hu_channel_history_entry_t **out,
                                                     size_t *out_count) {
    (void)ctx;
    if (!alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_INTERNAL;

    const char *sql = "SELECT m.is_from_me, m.text, "
                      "  datetime(m.date/1000000000 + 978307200, 'unixepoch', 'localtime') as ts, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj "
                      "   JOIN attachment a ON maj.attachment_id = a.ROWID "
                      "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "
                      "   AND (LOWER(a.filename) LIKE '%.mov' OR LOWER(a.filename) LIKE '%.mp4' "
                      "     OR LOWER(a.filename) LIKE '%.m4v')) > 0 AS has_video, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj2 "
                      "   JOIN attachment a2 ON maj2.attachment_id = a2.ROWID "
                      "   WHERE maj2.message_id = m.ROWID AND a2.filename IS NOT NULL "
                      "   AND (LOWER(a2.filename) LIKE '%.jpg' OR LOWER(a2.filename) LIKE '%.jpeg' "
                      "     OR LOWER(a2.filename) LIKE '%.png' OR LOWER(a2.filename) LIKE '%.heic' "
                      "     OR LOWER(a2.filename) LIKE '%.gif' OR LOWER(a2.filename) LIKE "
                      "'%.webp')) > 0 AS has_image, "
                      "  (SELECT COUNT(*) FROM message_attachment_join maj3 "
                      "   JOIN attachment a3 ON maj3.attachment_id = a3.ROWID "
                      "   WHERE maj3.message_id = m.ROWID AND a3.filename IS NOT NULL "
                      "   AND (LOWER(a3.filename) LIKE '%.caf' OR LOWER(a3.filename) LIKE '%.m4a' "
                      "     OR LOWER(a3.filename) LIKE '%.mp3' OR LOWER(a3.filename) LIKE '%.aac' "
                      "     OR LOWER(a3.filename) LIKE '%.opus')) > 0 AS has_audio, "
                      "  m.attributedBody, "
                      "  m.balloon_bundle_id, "
                      "  m.expressive_send_style_id "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.associated_message_type = 0 "
                      "ORDER BY m.date DESC LIMIT ?2";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);
    sqlite3_bind_int(stmt, 2, (int)(limit > 50 ? 50 : limit));

    if (limit > 50)
        limit = 50;
    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, limit * sizeof(*entries));
    if (!entries) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, limit * sizeof(*entries));
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        entries[count].from_me = sqlite3_column_int(stmt, 0) != 0;
        const char *txt = (const char *)sqlite3_column_text(stmt, 1);
        const char *ts = (const char *)sqlite3_column_text(stmt, 2);
        int has_video = sqlite3_column_int(stmt, 3);
        int has_image = sqlite3_column_int(stmt, 4);
        int has_audio = sqlite3_column_int(stmt, 5);

        /* macOS 15+: extract from attributedBody when text column is NULL */
        char attr_buf[4096];
        if ((!txt || txt[0] == '\0')) {
            const unsigned char *ab = sqlite3_column_blob(stmt, 6);
            int ab_len = sqlite3_column_bytes(stmt, 6);
            if (ab && ab_len > 0) {
                size_t extracted = hu_imessage_extract_attributed_body(ab, (size_t)ab_len, attr_buf,
                                                                       sizeof(attr_buf));
                if (extracted > 0)
                    txt = attr_buf;
            }
        }
        /* Sticker/Memoji classification from balloon_bundle_id (col 7) */
        const char *hist_balloon = (const char *)sqlite3_column_text(stmt, 7);
        if (hu_imessage_text_is_placeholder(txt)) {
            const char *label = hu_imessage_balloon_label(hist_balloon);
            if (label)
                txt = label;
        }
        /* Effect decoration from expressive_send_style_id (col 8) */
        const char *hist_effect = (const char *)sqlite3_column_text(stmt, 8);
        char hist_effect_buf[4200];
        if (txt && txt[0] != '\0') {
            const char *ename = hu_imessage_effect_name(hist_effect);
            if (ename) {
                snprintf(hist_effect_buf, sizeof(hist_effect_buf), "[Sent with %s] %s", ename, txt);
                txt = hist_effect_buf;
            }
        }
        if (txt && strlen(txt) > 0) {
            size_t tlen = strlen(txt);
            if (tlen >= sizeof(entries[0].text))
                tlen = sizeof(entries[0].text) - 1;
            memcpy(entries[count].text, txt, tlen);
            entries[count].text[tlen] = '\0';
        } else if (entries[count].from_me) {
            snprintf(entries[count].text, sizeof(entries[0].text), "[you replied]");
        } else {
            if (has_audio)
                snprintf(entries[count].text, sizeof(entries[0].text), "[Voice Message]");
            else if (has_video)
                snprintf(entries[count].text, sizeof(entries[0].text), "[Video]");
            else if (has_image)
                snprintf(entries[count].text, sizeof(entries[0].text), "[Photo]");
            else
                snprintf(entries[count].text, sizeof(entries[0].text), "[image or attachment]");
        }
        if (ts) {
            size_t tslen = strlen(ts);
            if (tslen >= sizeof(entries[0].timestamp))
                tslen = sizeof(entries[0].timestamp) - 1;
            memcpy(entries[count].timestamp, ts, tslen);
            entries[count].timestamp[tslen] = '\0';
        }
        count++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Results come in DESC order; reverse to chronological for caller */
    for (size_t i = 0; i < count / 2; i++) {
        hu_channel_history_entry_t tmp = entries[i];
        entries[i] = entries[count - 1 - i];
        entries[count - 1 - i] = tmp;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
#else
    (void)contact_id_len;
    (void)limit;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t imessage_get_response_constraints(void *ctx,
                                                    hu_channel_response_constraints_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->max_chars = 200;
    return HU_OK;
}

static char *imessage_vt_get_attachment_path(void *ctx, hu_allocator_t *alloc, int64_t message_id) {
    (void)ctx;
#ifndef HU_IS_TEST
    return hu_imessage_get_attachment_path(alloc, message_id);
#else
    (void)alloc;
    (void)message_id;
    return NULL;
#endif
}

static char *imessage_vt_get_latest_attachment_path(void *ctx, hu_allocator_t *alloc,
                                                    const char *contact_id, size_t contact_id_len) {
    (void)ctx;
#ifndef HU_IS_TEST
    return hu_imessage_get_latest_attachment_path(alloc, contact_id, contact_id_len);
#else
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    return NULL;
#endif
}

static bool imessage_vt_human_active_recently(void *ctx, const char *contact, size_t contact_len,
                                              int window_sec) {
#if defined(HU_ENABLE_SQLITE) && !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
    return hu_imessage_user_responded_recently(ctx, contact, contact_len, window_sec);
#else
    (void)ctx;
    (void)contact;
    (void)contact_len;
    (void)window_sec;
    return false;
#endif
}

static hu_error_t imessage_vt_build_reaction_context(void *ctx, hu_allocator_t *alloc,
                                                     const char *contact_id, size_t contact_id_len,
                                                     char **out, size_t *out_len) {
    (void)ctx;
    return hu_imessage_build_tapback_context(alloc, contact_id, contact_id_len, out, out_len);
}

static hu_error_t imessage_vt_build_read_receipt_context(void *ctx, hu_allocator_t *alloc,
                                                         const char *contact_id,
                                                         size_t contact_id_len, char **out,
                                                         size_t *out_len) {
    (void)ctx;
    return hu_imessage_build_read_receipt_context(alloc, contact_id, contact_id_len, out, out_len);
}

static hu_error_t imessage_mark_read(void *ctx, const char *contact_id, size_t contact_id_len) {
#ifdef HU_IS_TEST
    (void)ctx;
    (void)contact_id;
    (void)contact_id_len;
    return HU_OK;
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)contact_id;
    (void)contact_id_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c->alloc || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (contact_id_len > 4096)
        return HU_ERR_INVALID_ARGUMENT;
    size_t esc_cap = contact_id_len * 2 + 1;
    char *esc = (char *)c->alloc->alloc(c->alloc->ctx, esc_cap);
    if (!esc)
        return HU_ERR_OUT_OF_MEMORY;
    escape_for_applescript(esc, esc_cap, contact_id, contact_id_len);

    size_t script_cap = 512 + strlen(esc);
    char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
    if (!script) {
        c->alloc->free(c->alloc->ctx, esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(script, script_cap,
                     "tell application \"Messages\"\n"
                     "  set targetService to 1st service whose service type = iMessage\n"
                     "  set targetBuddy to buddy \"%s\" of targetService\n"
                     "  set targetChat to a reference to chat id (id of targetBuddy)\n"
                     "  read targetChat\n"
                     "end tell",
                     esc);
    if (n < 0 || (size_t)n >= script_cap) {
        c->alloc->free(c->alloc->ctx, script, script_cap);
        c->alloc->free(c->alloc->ctx, esc, esc_cap);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *argv[] = {"osascript", "-e", script, NULL};
    hu_run_result_t rr = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &rr);
    bool ok = (err == HU_OK && rr.success && rr.exit_code == 0);
    hu_run_result_free(c->alloc, &rr);
    c->alloc->free(c->alloc->ctx, script, script_cap);
    c->alloc->free(c->alloc->ctx, esc, esc_cap);
    return ok ? HU_OK : (err != HU_OK ? err : HU_ERR_INTERNAL);
#endif
}

static const hu_channel_vtable_t imessage_vtable = {
    .start = imessage_start,
    .stop = imessage_stop,
    .send = imessage_send,
    .name = imessage_name,
    .health_check = imessage_health_check,
    .send_event = NULL,
    .start_typing = imessage_start_typing,
    .stop_typing = imessage_stop_typing,
    .load_conversation_history = imessage_load_conversation_history,
    .get_response_constraints = imessage_get_response_constraints,
    .react = imessage_react,
    .get_attachment_path = imessage_vt_get_attachment_path,
    .human_active_recently = imessage_vt_human_active_recently,
    .get_latest_attachment_path = imessage_vt_get_latest_attachment_path,
    .build_reaction_context = imessage_vt_build_reaction_context,
    .build_read_receipt_context = imessage_vt_build_read_receipt_context,
    .mark_read = imessage_mark_read,
};

hu_error_t hu_imessage_create(hu_allocator_t *alloc, const char *default_target,
                              size_t default_target_len, const char *const *allow_from,
                              size_t allow_from_count, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->default_target = NULL;
    c->default_target_len = 0;
    c->allow_from = allow_from;
    c->allow_from_count = allow_from_count;
    if (default_target && default_target_len > 0) {
        c->default_target = (char *)alloc->alloc(alloc->ctx, default_target_len + 1);
        if (!c->default_target) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->default_target, default_target, default_target_len);
        c->default_target[default_target_len] = '\0';
        c->default_target_len = default_target_len;
    }
    /* Seed last_rowid: prefer persisted value from previous run (self-heal
     * after crash), fall back to current MAX(ROWID) minus optional lookback. */
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
    {
        int64_t persisted = imessage_load_rowid();
        const char *home_env = getenv("HOME");
        if (home_env) {
            char db_path[512];
            int dn = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home_env);
            if (dn > 0 && (size_t)dn < sizeof(db_path)) {
                sqlite3 *db = NULL;
                if (imessage_open_chatdb(db_path, &db) == SQLITE_OK) {
                    int64_t db_max = 0;
                    sqlite3_stmt *stmt = NULL;
                    if (sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL) ==
                        SQLITE_OK) {
                        if (sqlite3_step(stmt) == SQLITE_ROW)
                            db_max = sqlite3_column_int64(stmt, 0);
                        sqlite3_finalize(stmt);
                    }
                    if (persisted > 0 && persisted <= db_max) {
                        c->last_rowid = persisted;
                        hu_log_info("imessage", NULL,
                                    "resuming from persisted rowid=%lld (db max=%lld, "
                                    "recovering %lld messages)",
                                    (long long)persisted, (long long)db_max,
                                    (long long)(db_max - persisted));
                    } else {
                        c->last_rowid = db_max;
                        const char *lookback_env = getenv("HU_IMESSAGE_LOOKBACK");
                        if (lookback_env) {
                            long lb = strtol(lookback_env, NULL, 10);
                            if (lb > 0 && lb < 100 && c->last_rowid > lb)
                                c->last_rowid -= lb;
                        }
                    }
                    sqlite3_close(db);
                }
            }
        }
    }
#endif

    out->ctx = c;
    out->vtable = &imessage_vtable;
    return HU_OK;
}

bool hu_imessage_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    return c->default_target != NULL && c->default_target_len > 0;
}

void hu_imessage_set_use_imsg_cli(hu_channel_t *ch, bool use) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    c->use_imsg_cli = use;
}

void hu_imessage_set_loopback_handle(hu_channel_t *ch, const char *handle) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    c->loopback_handle = handle;
}

bool hu_imessage_watch_active(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    return c->imsg_watch_running;
#else
    return false;
#endif
}

void hu_imessage_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
        imsg_watch_stop(c);
#endif
        hu_allocator_t *a = c->alloc;
        if (c->default_target)
            a->free(a->ctx, c->default_target, c->default_target_len + 1);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#ifndef HU_IS_TEST
#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id) {
    if (!alloc || message_id <= 0)
        return NULL;

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return NULL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return NULL;

    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "WHERE maj.message_id = ?1 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, message_id);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        if (filename && filename[0]) {
            size_t len = strlen(filename);
            /* Expand ~ to home directory */
            if (len >= 1 && filename[0] == '~' && (len == 1 || filename[1] == '/')) {
                size_t home_len = strlen(home);
                size_t suffix_len = (len > 1) ? len - 1 : 0;
                size_t total = home_len + suffix_len + 1;
                path = (char *)alloc->alloc(alloc->ctx, total);
                if (path) {
                    memcpy(path, home, home_len);
                    if (suffix_len > 0)
                        memcpy(path + home_len, filename + 1, suffix_len);
                    path[total - 1] = '\0';
                }
            } else {
                path = hu_strndup(alloc, filename, len);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    /* Validate path is within Messages attachments directory */
    if (path && home) {
        char allowed_prefix[512];
        int prefix_len = snprintf(allowed_prefix, sizeof(allowed_prefix),
                                  "%s/Library/Messages/Attachments/", home);
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(allowed_prefix)) {
            if (strncmp(path, allowed_prefix, (size_t)prefix_len) != 0) {
                alloc->free(alloc->ctx, path, strlen(path) + 1);
                return NULL; /* Path outside allowed directory */
            }
        }
    }
    return path;
}
#else
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id) {
    (void)alloc;
    (void)message_id;
    return NULL;
}
#endif

#if defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len) {
    if (!alloc || !contact_id || contact_id_len == 0)
        return NULL;

    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return NULL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return NULL;

    /* Get the most recent message with attachment from this contact */
    const char *sql = "SELECT a.filename FROM attachment a "
                      "JOIN message_attachment_join maj ON maj.attachment_id = a.ROWID "
                      "JOIN message m ON maj.message_id = m.ROWID "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 0 "
                      "ORDER BY m.date DESC LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    char contact_buf[256];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, -1, NULL);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *filename = (const char *)sqlite3_column_text(stmt, 0);
        if (filename && filename[0]) {
            size_t len = strlen(filename);
            if (len >= 1 && filename[0] == '~' && (len == 1 || filename[1] == '/')) {
                size_t home_len = strlen(home);
                size_t suffix_len = (len > 1) ? len - 1 : 0;
                size_t total = home_len + suffix_len + 1;
                path = (char *)alloc->alloc(alloc->ctx, total);
                if (path) {
                    memcpy(path, home, home_len);
                    if (suffix_len > 0)
                        memcpy(path + home_len, filename + 1, suffix_len);
                    path[total - 1] = '\0';
                }
            } else {
                path = hu_strndup(alloc, filename, len);
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    /* Validate path is within Messages attachments directory */
    if (path && home) {
        char allowed_prefix[512];
        int prefix_len = snprintf(allowed_prefix, sizeof(allowed_prefix),
                                  "%s/Library/Messages/Attachments/", home);
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(allowed_prefix)) {
            if (strncmp(path, allowed_prefix, (size_t)prefix_len) != 0) {
                alloc->free(alloc->ctx, path, strlen(path) + 1);
                return NULL; /* Path outside allowed directory */
            }
        }
    }
    return path;
}
#else
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    return NULL;
}
#endif
#endif

/* ── iMessage polling via ~/Library/Messages/chat.db ──────────────────── */

hu_error_t hu_imessage_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count) {
    (void)alloc;
    if (!channel_ctx || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;
        /* Mirror the production heartbeat contract: a healthy idle watch
         * tick refreshes the success epoch. Tests use
         * `hu_imessage_test_set_watch_running` to drive this branch. */
        if (c->imsg_watch_running)
            imessage_record_poll_heartbeat(c, (int64_t)time(NULL));
        if (c->mock_count > 0) {
            size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
            for (size_t i = 0; i < n; i++) {
                memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
                memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
                msgs[i].message_id = (int64_t)(i + 1);
                msgs[i].is_group = c->mock_msgs[i].is_group;
                msgs[i].has_attachment = c->mock_msgs[i].has_attachment;
                msgs[i].has_video = c->mock_msgs[i].has_video;
                msgs[i].was_edited = c->mock_msgs[i].was_edited;
                msgs[i].was_unsent = c->mock_msgs[i].was_unsent;
                msgs[i].timestamp_sec = c->mock_msgs[i].timestamp_sec;
                memcpy(msgs[i].guid, c->mock_msgs[i].guid, 96);
                memcpy(msgs[i].reply_to_guid, c->mock_msgs[i].reply_to_guid, 96);
                memcpy(msgs[i].chat_id, c->mock_msgs[i].chat_id, 128);
            }
            *out_count = n;
            c->mock_count = 0;
            return HU_OK;
        }
        return HU_OK;
    }
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#elif !defined(HU_ENABLE_SQLITE)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)channel_ctx;

    /* Breaker tripped: short-circuit with a clean HU_OK / 0 messages so the
     * daemon's outer poll-loop does not log "[human] poll error" on every
     * tick. The breaker already emitted ONE explanatory error line; from
     * there on, the doctor + status file carry the truth. The breaker auto-
     * resets on the next successful chat.db open, so we still attempt the
     * occasional probe (every Nth tick) to detect FDA recovery. */
    if (c->circuit_breaker_tripped) {
        c->breaker_recovery_probe_counter++;
        /* ~30 ticks ≈ 30s at 1s poll cadence. Cheap enough; rare enough that
         * a still-revoked FDA does not regenerate log spam. */
        if (c->breaker_recovery_probe_counter < 30)
            return HU_OK;
        c->breaker_recovery_probe_counter = 0;
        /* Fall through and attempt one open; if it succeeds, breaker resets. */
    }

    /* When imsg watch is active, skip the SQL query if no new data arrived.
     * This avoids redundant queries while maintaining sub-second latency.
     * If the watch process died, attempt restart before falling back to SQL. */
    if (c->imsg_watch_running) {
        if (!imsg_watch_has_data(c)) {
            /* Two cases here:
             *   1. Read returned EAGAIN: watch is alive, just no new data.
             *      `imsg_watch_running` stays true. This idle tick IS a
             *      successful poll cycle — record the heartbeat so the
             *      doctor's stall threshold doesn't trip on quiet hours.
             *   2. Read returned EOF / error: `imsg_watch_has_data` already
             *      tore down the watch and cleared `imsg_watch_running`.
             *      Don't claim success; let the next poll restart it. */
            if (c->imsg_watch_running)
                imessage_record_poll_heartbeat(c, (int64_t)time(NULL));
            return HU_OK;
        }
    } else if (c->use_imsg_cli && imsg_cli_available(c)) {
        imsg_watch_start(c);
    }

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    int rc = imessage_open_chatdb(db_path, &db);
    if (rc != SQLITE_OK) {
        /* Log only when not yet circuit-broken; once tripped the breaker
         * already explained the situation in a single high-priority line. */
        if (!c->circuit_breaker_tripped)
            hu_log_error("imessage", NULL, "cannot open chat.db: error %d", rc);
        (void)imessage_record_open_result(c, rc, (int64_t)time(NULL));
        imessage_save_poll_status(c);
        return HU_ERR_IO;
    }

    /* Detect date_retracted column (macOS Ventura+) for unsend detection */
    bool has_date_retracted = false;
    {
        sqlite3_stmt *col_check = NULL;
        if (sqlite3_prepare_v2(db, "SELECT date_retracted FROM message LIMIT 0", -1, &col_check,
                               NULL) == SQLITE_OK) {
            has_date_retracted = true;
            sqlite3_finalize(col_check);
        }
    }

    /* If last_rowid was never seeded (e.g. FDA wasn't granted at startup),
     * seed it now to current max so we only pick up truly new messages. */
    if (c->last_rowid == 0) {
        sqlite3_stmt *seed = NULL;
        if (sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &seed, NULL) ==
            SQLITE_OK) {
            if (sqlite3_step(seed) == SQLITE_ROW)
                c->last_rowid = sqlite3_column_int64(seed, 0);
            sqlite3_finalize(seed);
        }
        hu_log_info("imessage", NULL,
                    "late-seeded last_rowid=%lld (only new messages will be processed)",
                    (long long)c->last_rowid);
        sqlite3_close(db);
        imessage_record_poll_success(c, (int64_t)time(NULL));
        imessage_save_poll_status(c);
        *out_count = 0;
        return HU_OK;
    }

    /* Column layout (0-indexed):
     *   0: ROWID, 1: guid, 2: text (COALESCE), 3: handle.id,
     *   4: participant_count, 5: has_image, 6: has_video, 7: has_audio,
     *   8: was_edited, 9: thread_originator_guid, 10: attributedBody,
     *   11: balloon_bundle_id, 12: expressive_send_style_id, 13: unix_ts,
     *   14: was_retracted (only when has_date_retracted, else absent),
     *   15: chat_id (from chat_message_join → chat.guid)
     *
     * Attachment type classification uses EXISTS (cheaper than COUNT). */

#define IMSG_POLL_SQL_BASE                                                                  \
    "SELECT m.ROWID, m.guid, "                                                              \
    "  COALESCE(m.text, "                                                                   \
    "    (SELECT CASE "                                                                     \
    "       WHEN EXISTS (SELECT 1 FROM message_attachment_join maja "                       \
    "             JOIN attachment aa ON maja.attachment_id = aa.ROWID "                     \
    "             WHERE maja.message_id = m.ROWID AND aa.filename IS NOT NULL "             \
    "             AND (LOWER(aa.filename) LIKE '%.caf' OR LOWER(aa.filename) LIKE '%.m4a' " \
    "               OR LOWER(aa.filename) LIKE '%.mp3' OR LOWER(aa.filename) LIKE '%.aac' " \
    "               OR LOWER(aa.filename) LIKE '%.opus')) "                                 \
    "       THEN '[Voice Message]' "                                                        \
    "       WHEN EXISTS (SELECT 1 FROM message_attachment_join majv "                       \
    "             JOIN attachment av ON majv.attachment_id = av.ROWID "                     \
    "             WHERE majv.message_id = m.ROWID AND av.filename IS NOT NULL "             \
    "             AND (LOWER(av.filename) LIKE '%.mov' OR LOWER(av.filename) LIKE '%.mp4' " \
    "               OR LOWER(av.filename) LIKE '%.m4v')) "                                  \
    "       THEN '[Video]' ELSE '[Photo]' END)) AS text, h.id, "                            \
    "  COALESCE("                                                                           \
    "    (SELECT COUNT(DISTINCT chj2.handle_id) FROM chat_message_join cmj "                \
    "     JOIN chat_handle_join chj2 ON chj2.chat_id = cmj.chat_id "                        \
    "     WHERE cmj.message_id = m.ROWID), 0) AS participant_count, "                       \
    "  EXISTS (SELECT 1 FROM message_attachment_join maj "                                  \
    "   JOIN attachment a ON maj.attachment_id = a.ROWID "                                  \
    "   WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "                         \
    "   AND (LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' "            \
    "     OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "            \
    "     OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp')) "          \
    "   AS has_image, "                                                                     \
    "  EXISTS (SELECT 1 FROM message_attachment_join maj2 "                                 \
    "   JOIN attachment a2 ON maj2.attachment_id = a2.ROWID "                               \
    "   WHERE maj2.message_id = m.ROWID AND a2.filename IS NOT NULL "                       \
    "   AND (LOWER(a2.filename) LIKE '%.mov' OR LOWER(a2.filename) LIKE '%.mp4' "           \
    "     OR LOWER(a2.filename) LIKE '%.m4v')) AS has_video, "                              \
    "  EXISTS (SELECT 1 FROM message_attachment_join maj3 "                                 \
    "   JOIN attachment a3 ON maj3.attachment_id = a3.ROWID "                               \
    "   WHERE maj3.message_id = m.ROWID AND a3.filename IS NOT NULL "                       \
    "   AND (LOWER(a3.filename) LIKE '%.caf' OR LOWER(a3.filename) LIKE '%.m4a' "           \
    "     OR LOWER(a3.filename) LIKE '%.mp3' OR LOWER(a3.filename) LIKE '%.aac' "           \
    "     OR LOWER(a3.filename) LIKE '%.opus')) AS has_audio, "                             \
    "  CASE WHEN m.date_edited > 0 THEN 1 ELSE 0 END AS was_edited, "                       \
    "  m.thread_originator_guid, "                                                          \
    "  m.attributedBody, "                                                                  \
    "  m.balloon_bundle_id, "                                                               \
    "  m.expressive_send_style_id, "                                                        \
    "  m.date / 1000000000 + 978307200 AS unix_ts"

#define IMSG_POLL_SQL_RETRACT ", CASE WHEN m.date_retracted > 0 THEN 1 ELSE 0 END AS was_retracted"

#define IMSG_POLL_SQL_CHAT_ID                       \
    ", (SELECT c.guid FROM chat_message_join cmj2 " \
    "   JOIN chat c ON cmj2.chat_id = c.ROWID "     \
    "   WHERE cmj2.message_id = m.ROWID LIMIT 1) AS chat_guid"

#define IMSG_POLL_SQL_FROM                                                              \
    " FROM message m "                                                                  \
    "JOIN handle h ON m.handle_id = h.ROWID "                                           \
    "WHERE (m.is_from_me = 0 OR (m.is_from_me = 1 AND h.id = ?3)) "                     \
    "AND m.associated_message_type = 0 "                                                \
    "AND m.ROWID > ?1 "                                                                 \
    "AND ((m.text IS NOT NULL AND LENGTH(m.text) > 0) "                                 \
    "     OR (m.attributedBody IS NOT NULL AND LENGTH(m.attributedBody) > 0) "          \
    "     OR (EXISTS (SELECT 1 FROM message_attachment_join maj "                       \
    "         JOIN attachment a ON maj.attachment_id = a.ROWID "                        \
    "         WHERE maj.message_id = m.ROWID AND a.filename IS NOT NULL "               \
    "         AND ((LOWER(a.filename) LIKE '%.jpg' OR LOWER(a.filename) LIKE '%.jpeg' " \
    "           OR LOWER(a.filename) LIKE '%.png' OR LOWER(a.filename) LIKE '%.heic' "  \
    "           OR LOWER(a.filename) LIKE '%.gif' OR LOWER(a.filename) LIKE '%.webp') " \
    "           OR (LOWER(a.filename) LIKE '%.mov' OR LOWER(a.filename) LIKE '%.mp4' "  \
    "             OR LOWER(a.filename) LIKE '%.m4v') "                                  \
    "           OR (LOWER(a.filename) LIKE '%.caf' OR LOWER(a.filename) LIKE '%.m4a' "  \
    "             OR LOWER(a.filename) LIKE '%.mp3' OR LOWER(a.filename) LIKE '%.aac' " \
    "             OR LOWER(a.filename) LIKE '%.opus')))) "                              \
    "     OR (m.balloon_bundle_id IS NOT NULL)) "                                       \
    "ORDER BY m.ROWID ASC LIMIT ?2"

    /* Build SQL variant based on available columns */
    char sql_buf[4096];
    int sql_len;
    if (has_date_retracted) {
        sql_len = snprintf(sql_buf, sizeof(sql_buf), "%s%s%s%s", IMSG_POLL_SQL_BASE,
                           IMSG_POLL_SQL_RETRACT, IMSG_POLL_SQL_CHAT_ID, IMSG_POLL_SQL_FROM);
    } else {
        sql_len = snprintf(sql_buf, sizeof(sql_buf), "%s%s%s", IMSG_POLL_SQL_BASE,
                           IMSG_POLL_SQL_CHAT_ID, IMSG_POLL_SQL_FROM);
    }
    if (sql_len < 0 || (size_t)sql_len >= sizeof(sql_buf)) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql_buf, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_log_error("imessage", NULL, "SQL prepare failed: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, c->last_rowid);
    sqlite3_bind_int(stmt, 2, (int)max_msgs);
    sqlite3_bind_text(stmt, 3, c->loopback_handle ? c->loopback_handle : "", -1, SQLITE_STATIC);

    const int col_retracted = has_date_retracted ? 14 : -1;
    const int col_chat_guid = has_date_retracted ? 15 : 14;

    size_t count = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_msgs) {
        int64_t rowid = sqlite3_column_int64(stmt, 0);
        const char *guid = (const char *)sqlite3_column_text(stmt, 1);
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        const char *handle = (const char *)sqlite3_column_text(stmt, 3);
        int participant_count = sqlite3_column_int(stmt, 4);
        int has_image = sqlite3_column_int(stmt, 5);
        int has_video = sqlite3_column_int(stmt, 6);
        int has_audio = sqlite3_column_int(stmt, 7);
        int was_edited = sqlite3_column_int(stmt, 8);
        const char *reply_to = (const char *)sqlite3_column_text(stmt, 9);

        /* macOS 15+: text column is often NULL while attributedBody has the
         * actual content. The COALESCE in the query substitutes '[Photo]' for
         * NULL text, so also try attributedBody when text is a placeholder. */
        char attr_text_buf[4096];
        if (!text || text[0] == '\0' || hu_imessage_text_is_placeholder(text)) {
            const unsigned char *attr_blob = sqlite3_column_blob(stmt, 10);
            int attr_len = sqlite3_column_bytes(stmt, 10);
            if (attr_blob && attr_len > 0) {
                size_t extracted = hu_imessage_extract_attributed_body(
                    attr_blob, (size_t)attr_len, attr_text_buf, sizeof(attr_text_buf));
                if (extracted > 0)
                    text = attr_text_buf;
            }
        }

        /* Sticker/Memoji detection via balloon_bundle_id (col 11).
         * Override text only when it's empty OR a COALESCE-generated generic label,
         * because Memoji/Sticker messages have no real text but COALESCE may set '[Photo]'. */
        const char *balloon_id = (const char *)sqlite3_column_text(stmt, 11);
        if (balloon_id && balloon_id[0]) {
            const char *label = hu_imessage_balloon_label(balloon_id);
            if (label && hu_imessage_text_is_placeholder(text))
                text = label;
        }

        /* Message effect detection via expressive_send_style_id (col 12) */
        const char *effect_id = (const char *)sqlite3_column_text(stmt, 12);
        char effect_buf[4200];
        if (text && text[0]) {
            const char *effect_name = hu_imessage_effect_name(effect_id);
            if (effect_name) {
                snprintf(effect_buf, sizeof(effect_buf), "[Sent with %s] %s", effect_name, text);
                text = effect_buf;
            }
        }

        int64_t msg_unix_ts = sqlite3_column_int64(stmt, 13);

        if (!text || !handle) {
            c->last_rowid = rowid;
            continue;
        }

        /* Skip messages that match content we recently sent (echo prevention) */
        if (imessage_was_sent_by_us(c, text, strlen(text))) {
            c->last_rowid = rowid;
            continue;
        }

        size_t handle_len = strlen(handle);
        if (c->allow_from_count > 0) {
            bool allowed = false;
            for (size_t i = 0; i < c->allow_from_count; i++) {
                const char *a = c->allow_from[i];
                if (!a)
                    continue;
                if (a[0] == '*' && a[1] == '\0') {
                    allowed = true;
                    break;
                }
                size_t a_len = strlen(a);
                if (a_len == handle_len && strncasecmp(handle, a, a_len) == 0) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                c->last_rowid = rowid;
                continue;
            }
        }
        size_t text_len = strlen(text);
        if (handle_len >= sizeof(msgs[count].session_key))
            handle_len = sizeof(msgs[count].session_key) - 1;
        if (text_len >= sizeof(msgs[count].content))
            text_len = sizeof(msgs[count].content) - 1;

        memcpy(msgs[count].session_key, handle, handle_len);
        msgs[count].session_key[handle_len] = '\0';
        memcpy(msgs[count].content, text, text_len);
        msgs[count].content[text_len] = '\0';
        msgs[count].message_id = rowid;
        msgs[count].is_group = (participant_count > 2);
        msgs[count].has_attachment = (has_image != 0 || has_audio != 0);
        msgs[count].has_video = (has_video != 0);
        if (guid && strlen(guid) > 0) {
            size_t g_len = strlen(guid);
            if (g_len >= sizeof(msgs[count].guid))
                g_len = sizeof(msgs[count].guid) - 1;
            memcpy(msgs[count].guid, guid, g_len);
            msgs[count].guid[g_len] = '\0';
        } else {
            msgs[count].guid[0] = '\0';
        }
        msgs[count].was_edited = (was_edited != 0);
        msgs[count].was_unsent =
            (col_retracted >= 0) ? (sqlite3_column_int(stmt, col_retracted) != 0) : false;
        if (reply_to && reply_to[0]) {
            size_t rt_len = strlen(reply_to);
            if (rt_len >= sizeof(msgs[count].reply_to_guid))
                rt_len = sizeof(msgs[count].reply_to_guid) - 1;
            memcpy(msgs[count].reply_to_guid, reply_to, rt_len);
            msgs[count].reply_to_guid[rt_len] = '\0';
        } else {
            msgs[count].reply_to_guid[0] = '\0';
        }
        msgs[count].timestamp_sec = msg_unix_ts;

        const char *chat_guid = (const char *)sqlite3_column_text(stmt, col_chat_guid);
        if (chat_guid && chat_guid[0]) {
            size_t cg_len = strlen(chat_guid);
            if (cg_len >= sizeof(msgs[count].chat_id))
                cg_len = sizeof(msgs[count].chat_id) - 1;
            memcpy(msgs[count].chat_id, chat_guid, cg_len);
            msgs[count].chat_id[cg_len] = '\0';
        } else {
            msgs[count].chat_id[0] = '\0';
        }

        c->last_rowid = rowid;
        count++;
        if (getenv("HU_DEBUG"))
            hu_log_info("imessage", NULL, "incoming handle=%s len=%zu", handle, text_len);
    }

    if (step_rc != SQLITE_DONE && step_rc != SQLITE_ROW)
        hu_log_error("imessage", NULL, "poll step unexpected result: %d (%s)", step_rc,
                     sqlite3_errmsg(db));

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (count > 0)
        imessage_save_rowid(c->last_rowid);

    if (count == 0 && getenv("HU_DEBUG"))
        hu_log_info("imessage", NULL, "poll: 0 messages (last_rowid=%lld)",
                    (long long)c->last_rowid);

    imessage_record_poll_success(c, (int64_t)time(NULL));
    imessage_save_poll_status(c);

    *out_count = count;
    return HU_OK;
#endif
}

/* ── Tenor JSON fallback + GIF download (v2) ─────────────────────────── */

/* ── Inline reply context: look up original message text by GUID ────── */

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len) {
    if (!alloc || !guid || guid_len == 0 || !out_text || out_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = 0;
    out_text[0] = '\0';

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_SUPPORTED;
    char db_path[512];
    if (snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home) < 0)
        return HU_ERR_INTERNAL;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    const char *sql =
        "SELECT m.text, m.attributedBody, m.balloon_bundle_id, m.expressive_send_style_id "
        "FROM message m WHERE m.guid = ? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_INTERNAL;
    }

    sqlite3_bind_text(stmt, 1, guid, (int)guid_len, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (text && text[0] != '\0') {
            *out_len = hu_imessage_copy_bounded(out_text, out_cap, text, strlen(text));
        } else {
            const unsigned char *ab = sqlite3_column_blob(stmt, 1);
            int ab_len = sqlite3_column_bytes(stmt, 1);
            if (ab && ab_len > 0) {
                *out_len =
                    hu_imessage_extract_attributed_body(ab, (size_t)ab_len, out_text, out_cap);
            }
        }
        /* Sticker/Memoji fallback when text is empty or a generic placeholder */
        if (hu_imessage_text_is_placeholder(out_text)) {
            const char *bid = (const char *)sqlite3_column_text(stmt, 2);
            const char *label = hu_imessage_balloon_label(bid);
            if (label)
                *out_len = hu_imessage_copy_bounded(out_text, out_cap, label, strlen(label));
        }
        /* Effect prefix when text is present */
        if (out_text[0] != '\0') {
            const char *eid = (const char *)sqlite3_column_text(stmt, 3);
            const char *ename = hu_imessage_effect_name(eid);
            if (ename) {
                char tmp[4200];
                int n2 = snprintf(tmp, sizeof(tmp), "[Sent with %s] %s", ename, out_text);
                if (n2 > 0) {
                    size_t avail = (size_t)n2 < sizeof(tmp) ? (size_t)n2 : sizeof(tmp) - 1;
                    *out_len = hu_imessage_copy_bounded(out_text, out_cap, tmp, avail);
                }
            }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
}
#elif HU_IS_TEST

static struct {
    char guid[96];
    char text[512];
} s_test_guid_store[16];
static size_t s_test_guid_count;

void hu_imessage_test_set_guid_lookup(const char *guid, const char *text) {
    if (!guid || !text || s_test_guid_count >= 16)
        return;
    size_t i = s_test_guid_count++;
    size_t gl = strlen(guid);
    if (gl > 95)
        gl = 95;
    memcpy(s_test_guid_store[i].guid, guid, gl);
    s_test_guid_store[i].guid[gl] = '\0';
    size_t tl = strlen(text);
    if (tl > 511)
        tl = 511;
    memcpy(s_test_guid_store[i].text, text, tl);
    s_test_guid_store[i].text[tl] = '\0';
}

void hu_imessage_test_clear_guid_lookups(void) {
    s_test_guid_count = 0;
}

hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len) {
    (void)alloc;
    if (!guid || guid_len == 0 || !out_text || out_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = 0;
    out_text[0] = '\0';
    for (size_t i = 0; i < s_test_guid_count; i++) {
        if (strlen(s_test_guid_store[i].guid) == guid_len &&
            memcmp(s_test_guid_store[i].guid, guid, guid_len) == 0) {
            *out_len = hu_imessage_copy_bounded(out_text, out_cap, s_test_guid_store[i].text,
                                                strlen(s_test_guid_store[i].text));
            return HU_OK;
        }
    }
    return HU_ERR_NOT_SUPPORTED;
}
#else
hu_error_t hu_imessage_lookup_message_by_guid(hu_allocator_t *alloc, const char *guid,
                                              size_t guid_len, char *out_text, size_t out_cap,
                                              size_t *out_len) {
    (void)alloc;
    (void)guid;
    (void)guid_len;
    if (out_text && out_cap > 0)
        out_text[0] = '\0';
    if (out_len)
        *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}
#endif

#if HU_IS_TEST
hu_error_t hu_imessage_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len) {
    return hu_imessage_test_inject_mock_ex(ch, session_key, session_key_len, content, content_len,
                                           false);
}

hu_error_t hu_imessage_test_inject_mock_ex(hu_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len, bool has_attachment) {
    return hu_imessage_test_inject_mock_ex2(ch, session_key, session_key_len, content, content_len,
                                            has_attachment, false);
}

hu_error_t hu_imessage_test_inject_mock_ex2(hu_channel_t *ch, const char *session_key,
                                            size_t session_key_len, const char *content,
                                            size_t content_len, bool has_attachment,
                                            bool has_video) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    memset(&c->mock_msgs[i], 0, sizeof(c->mock_msgs[i]));
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    c->mock_msgs[i].has_attachment = has_attachment;
    c->mock_msgs[i].has_video = has_video;
    return HU_OK;
}

hu_error_t hu_imessage_test_inject_mock_full(hu_channel_t *ch, const char *session_key,
                                             size_t session_key_len, const char *content,
                                             size_t content_len,
                                             const hu_imessage_test_msg_opts_t *opts) {
    if (!ch || !ch->ctx || !opts)
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    memset(&c->mock_msgs[i], 0, sizeof(c->mock_msgs[i]));
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    c->mock_msgs[i].has_attachment = opts->has_attachment;
    c->mock_msgs[i].has_video = opts->has_video;
    c->mock_msgs[i].is_group = opts->is_group;
    c->mock_msgs[i].was_edited = opts->was_edited;
    c->mock_msgs[i].was_unsent = opts->was_unsent;
    c->mock_msgs[i].timestamp_sec = opts->timestamp_sec;
    if (opts->guid && opts->guid[0]) {
        size_t gl = strlen(opts->guid);
        if (gl > 95)
            gl = 95;
        memcpy(c->mock_msgs[i].guid, opts->guid, gl);
        c->mock_msgs[i].guid[gl] = '\0';
    }
    if (opts->reply_to_guid && opts->reply_to_guid[0]) {
        size_t rl = strlen(opts->reply_to_guid);
        if (rl > 95)
            rl = 95;
        memcpy(c->mock_msgs[i].reply_to_guid, opts->reply_to_guid, rl);
        c->mock_msgs[i].reply_to_guid[rl] = '\0';
    }
    if (opts->chat_id && opts->chat_id[0]) {
        size_t cl = strlen(opts->chat_id);
        if (cl > 127)
            cl = 127;
        memcpy(c->mock_msgs[i].chat_id, opts->chat_id, cl);
        c->mock_msgs[i].chat_id[cl] = '\0';
    }
    return HU_OK;
}

void hu_imessage_test_store_guid_text(hu_channel_t *ch, const char *guid, const char *text) {
    if (!ch || !ch->ctx || !guid || !text)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (c->mock_guid_count >= 8)
        return;
    size_t i = c->mock_guid_count++;
    size_t gl = strlen(guid);
    if (gl > 95)
        gl = 95;
    memcpy(c->mock_guid_store[i], guid, gl);
    c->mock_guid_store[i][gl] = '\0';
    (void)text;
}

const char *hu_imessage_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}

void hu_imessage_test_get_last_reaction(hu_channel_t *ch, hu_reaction_type_t *out_reaction,
                                        int64_t *out_message_id) {
    if (!ch || !ch->ctx || !out_reaction || !out_message_id)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    *out_reaction = c->last_reaction;
    *out_message_id = c->last_reaction_message_id;
}

size_t hu_imessage_test_get_last_media_count(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return 0;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    return c->last_media_count;
}
#endif
