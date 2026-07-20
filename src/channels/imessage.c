#include "human/channels/imessage.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/channel_loop.h"
#include "human/channels/imessage_caps.h" /* native capability gate (T0.4) */
#include "human/channels/imessage_reply.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/io_secure.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/observability/validator_telemetry.h"
#include "human/persona.h"
#ifndef HU_CODENAME
#define HU_CODENAME "human"
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Threaded-reply parent matching (BUG #3) ─────────────────────────────
 * Pure predicate, defined OUTSIDE the AX `#if` so it is compiled (and unit-
 * testable) in every build, then called from ax_find_message_group() inside
 * the macOS-only AX code.
 *
 * The AX accessibility tree exposes each message bubble's text via its
 * description string (which also embeds sender/timestamp). To thread a reply
 * we locate the bubble whose text contains the parent message's prefix
 * (parent_guid_to_text_prefix: the first ~32 chars of the parent body).
 *
 * The original match used raw strstr(), which matches the prefix ANYWHERE —
 * including mid-token inside an unrelated message — so it could resolve to the
 * WRONG parent. This predicate instead requires the prefix to begin at a WORD
 * BOUNDARY (string start, or after a non-alphanumeric byte).
 *
 * Critically, the TRAILING edge is left UNCONSTRAINED: the parent prefix is
 * truncated at ~32 chars and routinely ends mid-word, so a both-sided
 * word-boundary match (e.g. hu_str_contains_word_ci) would spuriously FAIL and
 * break currently-working threading. Leading-boundary-only is the safe
 * tightening: it kills the mid-token false match while still matching the real
 * body (which begins at a boundary after the description's sender/timestamp
 * separator).
 *
 * NOTE: the identical-32-char-prefix collision (two messages sharing the same
 * leading 32 chars) is NOT solved here — both still match. That case is
 * mitigated by ax_find_message_group walking children most-recent-first; the
 * robust fix (longer prefix or GUID match) is future work. Live-macOS
 * validation against a real AX tree is required before trusting this. */
static inline bool imsg_is_alnum_byte(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool hu_imessage_desc_prefix_match(const char *haystack, const char *prefix) {
    if (!haystack || !prefix || !prefix[0])
        return false;
    for (const char *p = strstr(haystack, prefix); p != NULL; p = strstr(p + 1, prefix)) {
        if (p == haystack || !imsg_is_alnum_byte(p[-1]))
            return true; /* prefix begins at a word boundary */
    }
    return false;
}

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <libproc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <sys/wait.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif
#endif

#define HU_IMESSAGE_SENT_RING_SIZE  32
#define HU_IMESSAGE_SENT_PREFIX_LEN 256
#define HU_IMESSAGE_ROWID_FILE      ".human/imessage.rowid"
#define HU_IMESSAGE_STATUS_FILE     ".human/imessage.poll_status"

/* hu_imessage_extract_attributed_body now lives in src/util/typedstream.c
 * (unconditionally compiled) so unconditional callers like
 * src/channels/imessage_ingest.c resolve it even when HU_HAS_IMESSAGE=OFF.
 * The prototype remains in include/human/channels/imessage.h. */

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

#ifdef HU_IS_TEST
/* Test-only: stub function for send interception. When set, replaces osascript send. */
static hu_imessage_test_send_stub_fn g_imessage_test_send_stub = NULL;
#endif

typedef struct hu_imessage_ctx {
    hu_allocator_t *alloc;
    char *default_target;
    size_t default_target_len;
    bool running;
    int64_t last_rowid;
    const char *const *allow_from;
    size_t allow_from_count;
    /* Persona: borrowed pointer for overlay-aware rendering. */
    const hu_persona_t *persona;
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
    /* US-9.3: courtesy-reply config + one-shot AC-9.3.3 BUSY warn gate.
     * Always compiled so tests can drive the path regardless of platform. */
    bool courtesy_replies_enabled;
    bool chatdb_busy_log_emitted;
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
    /* US-9.3: mirror of the last courtesy reply sent by the poll loop.
     * Parallel to last_message so tests can disambiguate poller-emitted
     * courtesy replies from agent-driven sends. */
    char last_courtesy_message[4096];
    size_t last_courtesy_message_len;
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

/* ── Circuit breaker / status: ctx-dependent helpers (always compiled) ── */

/* Persist the channel's poll status to disk. Best-effort: silently no-ops if
 * HOME is unset or the file cannot be written. The format is intentionally a
 * tiny, hand-emitted JSON so it can be inspected with `cat` and parsed by the
 * doctor command without pulling in a JSON library at this layer. Creates
 * $HOME/.human if it does not exist (matches the rowid file's lifetime
 * assumption). */
static void imessage_save_poll_status(const hu_imessage_ctx_t *c) {
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
static bool imessage_record_open_result(hu_imessage_ctx_t *c, int rc, int64_t now) {
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

static void imessage_record_poll_success(hu_imessage_ctx_t *c, int64_t now) {
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
static void imessage_record_poll_heartbeat(hu_imessage_ctx_t *c, int64_t now)
    __attribute__((unused));
static void imessage_record_poll_heartbeat(hu_imessage_ctx_t *c, int64_t now) {
    if (!c)
        return;
    imessage_record_poll_success(c, now);
    imessage_save_poll_status(c);
}

/* ── US-9.3: non-allowlisted-sender courtesy reply ───────────────────────
 *
 * The pure predicate, text builder, and dedup-log helpers below sit at the
 * top of the file so the production poll loop and the test seam reach the
 * same symbols. The decision is intentionally factored out per
 * `.claude/rules/security-predicate-extraction.md`: the poll loop runs
 * inside a (hard-to-test) fork-and-SQL boundary, but the decision itself
 * is four facts → one bool. Tests pin all 16 rows without forking. */

bool hu_imessage_should_courtesy_reply(bool allowlist_has_handle, bool dedup_already_replied,
                                       bool courtesy_replies_enabled,
                                       uint32_t aggregate_today_count) {
    if (!courtesy_replies_enabled)
        return false;
    if (allowlist_has_handle)
        return false;
    if (dedup_already_replied)
        return false;
    if (aggregate_today_count >= (uint32_t)HU_IMESSAGE_COURTESY_DAILY_CAP)
        return false;
    return true;
}

size_t hu_imessage_build_courtesy_reply(const char *persona_name, const char *owner_display_name,
                                        char *out, size_t out_cap) {
    if (!out || out_cap < 32)
        return 0;
    /* Defensive: NEVER interpolate raw handles ("+1..." or "...@...").
     * The builder takes DISPLAY NAMES; the caller's responsibility is to
     * pass display names, not phone/email handles. We additionally sanity-
     * check inputs and fall back to generic phrasing if they look like a
     * handle, but the primary defense is the API shape. */
    const char *who = "the human assistant";
    char persona_buf[64] = {0};
    if (persona_name && persona_name[0]) {
        bool looks_like_handle = false;
        for (size_t i = 0; persona_name[i] && i < sizeof(persona_buf) - 1; i++) {
            char ch = persona_name[i];
            if (ch == '+' || ch == '@')
                looks_like_handle = true;
            persona_buf[i] = ch;
        }
        if (!looks_like_handle)
            who = persona_buf;
    }
    const char *owner = "the operator";
    char owner_buf[64] = {0};
    if (owner_display_name && owner_display_name[0]) {
        bool looks_like_handle = false;
        for (size_t i = 0; owner_display_name[i] && i < sizeof(owner_buf) - 1; i++) {
            char ch = owner_display_name[i];
            if (ch == '+' || ch == '@')
                looks_like_handle = true;
            owner_buf[i] = ch;
        }
        if (!looks_like_handle)
            owner = owner_buf;
    }
    int n = snprintf(out, out_cap,
                     "Hi — I'm %s, a human assistant running locally on %s's Mac. "
                     "To chat with me, please ask %s to add you to my allowlist. "
                     "(This is an automated one-time courtesy reply.)",
                     who, owner, owner);
    if (n < 0)
        return 0;
    if ((size_t)n >= out_cap) {
        out[out_cap - 1] = '\0';
        return out_cap - 1;
    }
    return (size_t)n;
}

bool hu_imessage_courtesy_log_path(char *buf, size_t cap) {
    if (!buf || cap < 16)
        return false;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    int n = snprintf(buf, cap, "%s/.human/imessage_courtesy.log", home);
    return n > 0 && (size_t)n < cap;
}

/* Parse a single dedup-log line of the form "<bucket> <handle>\n".
 * Returns true on success and writes bucket + handle (handle is a pointer
 * into `line` after the space). The line is mutated (NUL inserted). */
static bool imessage_courtesy_parse_line(char *line, int64_t *out_bucket, const char **out_handle) {
    if (!line || !out_bucket || !out_handle)
        return false;
    char *sp = strchr(line, ' ');
    if (!sp)
        return false;
    *sp = '\0';
    char *endp = NULL;
    long long b = strtoll(line, &endp, 10);
    if (!endp || *endp != '\0' || b < 0)
        return false;
    char *handle = sp + 1;
    /* Trim trailing newline. */
    size_t hl = strlen(handle);
    while (hl > 0 && (handle[hl - 1] == '\n' || handle[hl - 1] == '\r')) {
        handle[hl - 1] = '\0';
        hl--;
    }
    if (hl == 0)
        return false;
    *out_bucket = (int64_t)b;
    *out_handle = handle;
    return true;
}

bool hu_imessage_courtesy_dedup_check(const char *handle, int64_t bucket) {
    if (!handle || !handle[0])
        return false;
    char path[512];
    if (!hu_imessage_courtesy_log_path(path, sizeof(path)))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char buf[256];
        size_t ll = strlen(line);
        if (ll >= sizeof(buf))
            continue;
        memcpy(buf, line, ll + 1);
        int64_t b = 0;
        const char *h = NULL;
        if (!imessage_courtesy_parse_line(buf, &b, &h))
            continue;
        if (b == bucket && strcasecmp(h, handle) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

uint32_t hu_imessage_courtesy_aggregate_count(int64_t bucket) {
    char path[512];
    if (!hu_imessage_courtesy_log_path(path, sizeof(path)))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    uint32_t count = 0;
    while (fgets(line, sizeof(line), f)) {
        char buf[256];
        size_t ll = strlen(line);
        if (ll >= sizeof(buf))
            continue;
        memcpy(buf, line, ll + 1);
        int64_t b = 0;
        const char *h = NULL;
        if (!imessage_courtesy_parse_line(buf, &b, &h))
            continue;
        if (b == bucket && count < UINT32_MAX)
            count++;
    }
    fclose(f);
    return count;
}

/* Ensure the parent directory of the dedup log exists. Silently no-ops if
 * HOME is unset or mkdir fails (the caller's fopen will then fail too). */
static void imessage_courtesy_ensure_dir(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return;
    (void)mkdir(home, 0700);
    char dir[512];
    int n = snprintf(dir, sizeof(dir), "%s/.human", home);
    if (n > 0 && (size_t)n < sizeof(dir))
        (void)mkdir(dir, 0700);
}

void hu_imessage_courtesy_dedup_record(const char *handle, int64_t bucket) {
    if (!handle || !handle[0])
        return;
    imessage_courtesy_ensure_dir();
    char path[512];
    if (!hu_imessage_courtesy_log_path(path, sizeof(path)))
        return;
    /* Append the new entry. 0600 secret-class permissions. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    char rec[256];
    int n = snprintf(rec, sizeof(rec), "%lld %s\n", (long long)bucket, handle);
    if (n > 0 && (size_t)n < sizeof(rec)) {
        /* Best-effort journal write — `(void)` cast alone doesn't silence
         * GCC -Werror=unused-result (gcc-2.42 + glibc-2.39 specifically),
         * so check the return explicitly. Truncated/failed writes mean
         * the activity record is dropped this round; not fatal. */
        ssize_t ignored = write(fd, rec, (size_t)n);
        (void)ignored;
    }
    (void)close(fd);
    /* Truncate-tail if file has grown beyond the max-lines bound. Cheap
     * because the file is small (~16KB at the cap). Lock optimistically;
     * skip truncation on failure rather than spin. */
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    /* Best-effort advisory lock; non-fatal on failure (truncation just
     * deferred to the next record). */
#if defined(LOCK_EX) && defined(LOCK_NB)
    int trunc_fd = fileno(f);
    if (trunc_fd >= 0 && flock(trunc_fd, LOCK_EX | LOCK_NB) != 0) {
        fclose(f);
        return;
    }
#endif
    /* Slurp last N lines. We do a two-pass: count, then keep tail. */
    size_t line_count = 0;
    char tmp[256];
    while (fgets(tmp, sizeof(tmp), f))
        line_count++;
    if (line_count <= HU_IMESSAGE_COURTESY_LOG_MAX_LINES) {
        fclose(f);
        return;
    }
    size_t skip = line_count - HU_IMESSAGE_COURTESY_LOG_MAX_LINES;
    rewind(f);
    /* Re-skip the first `skip` lines. */
    for (size_t i = 0; i < skip; i++) {
        if (!fgets(tmp, sizeof(tmp), f)) {
            fclose(f);
            return;
        }
    }
    /* Buffer the tail in memory. 256 * 256 = 64KB worst case. */
    char *buf = (char *)malloc(HU_IMESSAGE_COURTESY_LOG_MAX_LINES * 256);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t buf_len = 0;
    while (fgets(tmp, sizeof(tmp), f)) {
        size_t tlen = strlen(tmp);
        if (buf_len + tlen >= HU_IMESSAGE_COURTESY_LOG_MAX_LINES * 256)
            break;
        memcpy(buf + buf_len, tmp, tlen);
        buf_len += tlen;
    }
    fclose(f);
    /* Atomically replace via tmp + rename. */
    char tmp_path[600];
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path)) {
        free(buf);
        return;
    }
    int tfd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tfd < 0) {
        free(buf);
        return;
    }
    if (buf_len > 0) {
        /* Best-effort tmp-file write before rename. Same -Werror=
         * unused-result note as above — `(void)` cast doesn't silence
         * the diagnostic on gcc-2.42, so assign-and-discard. */
        ssize_t ignored = write(tfd, buf, buf_len);
        (void)ignored;
    }
    (void)fsync(tfd);
    (void)close(tfd);
    (void)rename(tmp_path, path);
    free(buf);
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

/* Forward declarations for the native Messages.app bridge (defined later). */
static void ax_open_conversation(const char *recipient, size_t recipient_len);
static bool ax_start_typing(const char *target, size_t target_len);
static bool ax_stop_typing(void);
#ifdef HU_IMESSAGE_TAPBACK_ENABLED
static bool ax_tapback(const char *content_prefix, int row_offset, const char *tapback_label);
#endif
static bool imcore_init(hu_imessage_ctx_t *c);
static bool imcore_start_typing(hu_imessage_ctx_t *c, const char *recipient, size_t recipient_len);
static bool imcore_stop_typing(hu_imessage_ctx_t *c, const char *recipient, size_t recipient_len);

static uint32_t imessage_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static void imessage_record_sent(hu_imessage_ctx_t *c, const char *msg, size_t msg_len) {
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
static bool imessage_was_sent_by_us(hu_imessage_ctx_t *c, const char *text, size_t text_len) {
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
static int imessage_open_chatdb(const char *db_path, sqlite3 **db_out) {
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

#if defined(__APPLE__) && defined(__MACH__) && !HU_IS_TEST
/* Check if the imsg CLI (steipete/imsg) is available on $PATH.
 * Caches the result after first check. Only compiled in non-test macOS builds
 * since all callers are behind !HU_IS_TEST guards. */
static bool imsg_cli_available(hu_imessage_ctx_t *c) {
    if (!c || !c->alloc)
        return false;
    if (c->imsg_cli_checked)
        return c->has_imsg_cli;
    c->imsg_cli_checked = true;
    c->has_imsg_cli = false;
    const char *argv[] = {"which", "imsg", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
    if (err == HU_OK && result.success && result.exit_code == 0)
        c->has_imsg_cli = true;
    hu_run_result_free(c->alloc, &result);
    if (c->has_imsg_cli && getenv("HU_DEBUG"))
        hu_log_info("imessage", NULL, "imsg CLI detected on $PATH");
    return c->has_imsg_cli;
}
/* ── imsg watch subprocess (event-driven poll trigger) ─────────────── */

static void imsg_watch_start(hu_imessage_ctx_t *c) {
    if (!c || c->imsg_watch_running || !c->use_imsg_cli || !imsg_cli_available(c))
        return;
    if (c->circuit_breaker_tripped) {
        /* Refuse to respawn while breaker is tripped; the watch process would
         * just exit immediately on its own AUTH read of chat.db. */
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return;

    char rowid_str[32];
    snprintf(rowid_str, sizeof(rowid_str), "%lld", (long long)c->last_rowid);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("imsg", "imsg", "watch", "--json", "--since-rowid", rowid_str, NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    c->imsg_watch_pid = pid;
    c->imsg_watch_fd = pipefd[0];
    c->imsg_watch_running = true;
    hu_log_info("imessage", NULL, "started imsg watch (pid=%d, since-rowid=%s)", (int)pid,
                rowid_str);

    /* Watch path is the steady-state happy path; once it spawns, the SQL poll
     * function returns OK early without saving status. Without this update,
     * the persisted status file stays in whatever state the LAST SQL poll
     * left it (e.g. TRIPPED from a previous run before FDA was granted),
     * and `human doctor imessage` lies about daemon health. Record this
     * spawn as a successful poll so the status file reflects "watch active,
     * healthy" — this also auto-resets the breaker on recovery from a
     * previously-tripped state. */
    imessage_record_poll_success(c, (int64_t)time(NULL));
    imessage_save_poll_status(c);
}

/* Only called from the SQLite/Apple poll path below; without those flags
 * the function is dead code and -Werror=unused-function fires on minimal
 * Linux builds (HU_ENABLE_SQLITE=OFF + non-Apple host). */
__attribute__((unused)) static bool imsg_watch_has_data(hu_imessage_ctx_t *c) {
    if (!c->imsg_watch_running || c->imsg_watch_fd < 0)
        return false;

    char drain[4096];
    bool got_data = false;
    for (;;) {
        ssize_t n = read(c->imsg_watch_fd, drain, sizeof(drain));
        if (n > 0) {
            got_data = true;
            continue;
        }
        if (n == 0) {
            c->imsg_watch_running = false;
            close(c->imsg_watch_fd);
            c->imsg_watch_fd = -1;
            waitpid(c->imsg_watch_pid, NULL, WNOHANG);
            hu_log_info("imessage", NULL, "imsg watch exited, falling back to timer-based polling");
            return got_data;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return got_data;
        /* Unexpected read error — tear down watch to avoid zombie state */
        hu_log_error("imessage", NULL, "imsg watch pipe read error (errno=%d), tearing down",
                     errno);
        c->imsg_watch_running = false;
        close(c->imsg_watch_fd);
        c->imsg_watch_fd = -1;
        waitpid(c->imsg_watch_pid, NULL, WNOHANG);
        return got_data;
    }
}

static void imsg_watch_stop(hu_imessage_ctx_t *c) {
    if (!c || !c->imsg_watch_running)
        return;
    kill(c->imsg_watch_pid, SIGTERM);
    /* Non-blocking wait with retries to avoid hanging if child ignores SIGTERM */
    for (int i = 0; i < 10; i++) {
        if (waitpid(c->imsg_watch_pid, NULL, WNOHANG) != 0)
            goto reaped;
        usleep(100000);
    }
    kill(c->imsg_watch_pid, SIGKILL);
    waitpid(c->imsg_watch_pid, NULL, 0);
reaped:
    if (c->imsg_watch_fd >= 0) {
        close(c->imsg_watch_fd);
        c->imsg_watch_fd = -1;
    }
    c->imsg_watch_running = false;
    hu_log_info("imessage", NULL, "stopped imsg watch");
}

/* ── imsg chats target validation ─────────────────────────────────── */

static bool imsg_validate_target(hu_imessage_ctx_t *c) {
    if (!c || !c->default_target || c->default_target_len == 0)
        return false;
    if (c->imsg_target_validated)
        return true;

    const char *argv[] = {"imsg", "chats", "--json", "--limit", "100", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_with_timeout(c->alloc, argv, NULL, 262144, 10, &result);
    if (err != HU_OK || !result.success || result.exit_code != 0) {
        hu_run_result_free(c->alloc, &result);
        hu_log_info("imessage", NULL,
                    "imsg chats failed — cannot validate target (will retry next poll)");
        return false;
    }
    c->imsg_target_validated = true;

    bool found = false;
    if (result.stdout_buf && result.stdout_len >= c->default_target_len) {
        for (size_t i = 0; i <= result.stdout_len - c->default_target_len; i++) {
            if (memcmp(result.stdout_buf + i, c->default_target, c->default_target_len) == 0) {
                found = true;
                break;
            }
        }
    }
    hu_run_result_free(c->alloc, &result);

    if (!found)
        hu_log_info("imessage", NULL,
                    "target '%.*s' not found in active chats (imsg chats); "
                    "first message may create a new conversation",
                    (int)c->default_target_len, c->default_target);
    else if (getenv("HU_DEBUG"))
        hu_log_info("imessage", NULL, "target '%.*s' validated via imsg chats",
                    (int)c->default_target_len, c->default_target);
    return found;
}

/* ── Native-capability probe cache (T0.4) ───────────────────────────────
 * One `imsg status` per process. Everything advanced gates on this; when the
 * bridge is absent (SIP on) every native verb declines and the caller degrades
 * honestly — never UI puppetry, never a green bubble. */
static const hu_imessage_caps_t *imsg_caps_cached(hu_imessage_ctx_t *c) {
    static hu_imessage_caps_t caps;
    static bool probed = false;
    if (!c || !c->alloc)
        return NULL;
    if (!probed) {
        probed = true;
        (void)hu_imessage_caps_probe(c->alloc, &caps);
        char desc[160];
        hu_imessage_caps_describe(&caps, desc, sizeof(desc));
        hu_log_info("imessage", NULL, "%s", desc);
    }
    return &caps;
}

/* Resolve a chat.db message ROWID to its GUID (the bridge verbs address
 * messages by GUID, not rowid). Returns false when unavailable. */
#if defined(HU_ENABLE_SQLITE)
/* Open the user's chat.db read-only. Single place that builds the path, so the
 * boilerplate is not repeated per query helper. Returns NULL on any failure. */
static sqlite3 *imsg_open_user_chatdb(void) {
    const char *home_env = getenv("HOME");
    if (!home_env)
        return NULL;
    char db_p[512];
    int dp = snprintf(db_p, sizeof(db_p), "%s/Library/Messages/chat.db", home_env);
    if (dp <= 0 || (size_t)dp >= sizeof(db_p))
        return NULL;
    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_p, &db) != SQLITE_OK)
        return NULL;
    return db;
}
#endif

/* Run a single-TEXT-column chat.db query bound to one int64 parameter and copy
 * the result into `out`. Shared by every rowid→GUID lookup below so the
 * open/prepare/bind/step/finalize/close boilerplate exists once. */
static bool imsg_query_text1(const char *sql, int64_t i64_param, const char *txt_param,
                             size_t txt_len, char *out, size_t out_cap) {
    if (!sql || !out || out_cap == 0)
        return false;
    out[0] = '\0';
#if defined(HU_ENABLE_SQLITE)
    sqlite3 *db = imsg_open_user_chatdb();
    if (!db)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (txt_param)
            sqlite3_bind_text(st, 1, txt_param, (int)txt_len, SQLITE_STATIC);
        else
            sqlite3_bind_int64(st, 1, i64_param);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *g = (const char *)sqlite3_column_text(st, 0);
            if (g)
                snprintf(out, out_cap, "%s", g);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
#else
    (void)i64_param;
    (void)txt_param;
    (void)txt_len;
#endif
    return out[0] != '\0';
}

/* Resolve a chat.db message ROWID to its GUID (the bridge verbs address
 * messages by GUID, not rowid). Returns false when unavailable. */
static bool imsg_lookup_guid_for_message_id(hu_imessage_ctx_t *c, int64_t message_id, char *out,
                                            size_t out_cap) {
    if (!c || message_id <= 0)
        return false;
    return imsg_query_text1("SELECT guid FROM message WHERE ROWID = ? LIMIT 1", message_id, NULL, 0,
                            out, out_cap);
}

/* Resolve the CHAT guid owning a message ROWID. `imsg tapback|send-rich|edit|
 * unsend` address the chat by GUID (`--chat`), unlike `imsg react` which takes
 * a chat rowid (`--chat-id`). Returns false when unavailable. */
static bool imsg_lookup_chat_guid_for_message_id(hu_imessage_ctx_t *c, int64_t message_id,
                                                 char *out, size_t out_cap) {
    if (!c || message_id <= 0)
        return false;
    return imsg_query_text1("SELECT c.guid FROM chat c "
                            "JOIN chat_message_join cmj ON cmj.chat_id = c.ROWID "
                            "WHERE cmj.message_id = ? LIMIT 1",
                            message_id, NULL, 0, out, out_cap);
}

#if defined(HU_ENABLE_SQLITE)
/* Run a one-column service query bound to `handle`, mapping the result through
 * the service parser. UNKNOWN on any failure (⇒ blue verdict HOLDs). */
static hu_imessage_service_t imsg_query_service(sqlite3 *db, const char *sql, const char *handle,
                                                size_t handle_len) {
    hu_imessage_service_t svc = HU_IMSG_SERVICE_UNKNOWN;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return svc;
    sqlite3_bind_text(st, 1, handle, (int)handle_len, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (s)
            svc = hu_imessage_service_from_string(s, strlen(s));
    }
    sqlite3_finalize(st);
    return svc;
}
#endif

/* Blue guard evidence (T0.1): the service of the most recent message with this
 * handle (freshest routing evidence) and the handle row's own service. Both
 * default to UNKNOWN when chat.db is unavailable, which makes the verdict HOLD. */
static void imsg_lookup_services_for_handle(const char *handle, size_t handle_len,
                                            hu_imessage_service_t *recent_out,
                                            hu_imessage_service_t *handle_out) {
    if (recent_out)
        *recent_out = HU_IMSG_SERVICE_UNKNOWN;
    if (handle_out)
        *handle_out = HU_IMSG_SERVICE_UNKNOWN;
    if (!handle || handle_len == 0)
        return;
#if defined(HU_ENABLE_SQLITE)
    sqlite3 *db = imsg_open_user_chatdb();
    if (!db)
        return;
    /* Freshest routing evidence: what service did the last message use?
     * Authoritative fallback: prefer an iMessage handle row when one exists. */
    if (recent_out)
        *recent_out = imsg_query_service(db,
                                         "SELECT m.service FROM message m "
                                         "JOIN handle h ON m.handle_id = h.ROWID "
                                         "WHERE h.id = ? AND m.service IS NOT NULL "
                                         "ORDER BY m.date DESC LIMIT 1",
                                         handle, handle_len);
    if (handle_out)
        *handle_out = imsg_query_service(db,
                                         "SELECT service FROM handle WHERE id = ? "
                                         "ORDER BY (service = 'iMessage') DESC LIMIT 1",
                                         handle, handle_len);
    sqlite3_close(db);
#endif
}

/* Resolve a handle (phone/email) to the chat GUID owning its most recent
 * message — the bridge verbs address chats by GUID. Declared in
 * human/channels/imessage_schema.h; defined here so it shares the single
 * chat.db query helper above (clone-ratchet discipline). */
bool hu_imessage_reply_chat_guid_for_handle(const char *handle, size_t handle_len, char *out,
                                            size_t out_cap) {
    if (!handle || handle_len == 0)
        return false;
    return imsg_query_text1("SELECT c.guid FROM chat c "
                            "JOIN chat_message_join cmj ON cmj.chat_id = c.ROWID "
                            "JOIN message m ON m.ROWID = cmj.message_id "
                            "WHERE c.chat_identifier = ? ORDER BY m.date DESC LIMIT 1",
                            0, handle, handle_len, out, out_cap);
}

/* ── imsg react helper (shared by both tapback-enabled and tapback-disabled paths) ── */

static bool imsg_try_react(hu_imessage_ctx_t *c, int64_t message_id, hu_reaction_type_t reaction) {
    if (!c || !c->alloc || message_id <= 0)
        return false;
    const char *tapback_name = hu_imessage_reaction_to_tapback_name(reaction);
    if (!tapback_name)
        return false;

    char chat_rowid_str[32] = {0};
#if defined(HU_ENABLE_SQLITE)
    const char *home_env = getenv("HOME");
    if (home_env) {
        char db_p[512];
        int dp = snprintf(db_p, sizeof(db_p), "%s/Library/Messages/chat.db", home_env);
        if (dp > 0 && (size_t)dp < sizeof(db_p)) {
            sqlite3 *db = NULL;
            if (imessage_open_chatdb(db_p, &db) == SQLITE_OK) {
                sqlite3_stmt *cs = NULL;
                if (sqlite3_prepare_v2(db,
                                       "SELECT cmj.chat_id FROM chat_message_join cmj "
                                       "WHERE cmj.message_id = ? LIMIT 1",
                                       -1, &cs, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(cs, 1, message_id);
                    if (sqlite3_step(cs) == SQLITE_ROW) {
                        int64_t rowid = sqlite3_column_int64(cs, 0);
                        snprintf(chat_rowid_str, sizeof(chat_rowid_str), "%lld", (long long)rowid);
                    }
                    sqlite3_finalize(cs);
                }
                sqlite3_close(db);
            }
        }
    }
#endif
    if (!chat_rowid_str[0])
        return false;

    /* T0.2 (docs/plans/2026-07-19-native-imessage): prefer the IMCore-bridge
     * tapback when the bridge is live — that is a REAL reaction
     * (associatedMessageType), not an AppleScript approximation. `imsg react`
     * remains the SIP-on path. Both take the same kind vocabulary. */
    if (hu_imessage_caps_allows(imsg_caps_cached(c), HU_IMSG_VERB_REACT)) {
        char msg_guid[128] = {0};
        char chat_guid[128] = {0};
        if (imsg_lookup_guid_for_message_id(c, message_id, msg_guid, sizeof(msg_guid)) &&
            imsg_lookup_chat_guid_for_message_id(c, message_id, chat_guid, sizeof(chat_guid))) {
            const char *tb_argv[] = {"imsg",   "tapback", "--chat",     chat_guid, "--message",
                                     msg_guid, "--kind",  tapback_name, NULL};
            bool tbok = hu_imsg_run_ok(c->alloc, tb_argv, 15);
            if (tbok) {
                hu_log_info("imessage", NULL, "tapback sent via IMCore bridge (native)");
                return true;
            }
            hu_log_info("imessage", NULL, "bridge tapback failed; falling back to imsg react");
        }
    }

    const char *react_argv[] = {"imsg",       "react",      "--chat-id", chat_rowid_str,
                                "--reaction", tapback_name, NULL};
    hu_run_result_t rr = {0};
    hu_error_t re = hu_process_run_with_timeout(c->alloc, react_argv, NULL, 65536, 15, &rr);
    bool rok = (re == HU_OK && rr.success && rr.exit_code == 0);
    if (!rok)
        hu_log_info(
            "imessage", NULL, "imsg react failed (exit=%d stdout=%.*s stderr=%.*s)", rr.exit_code,
            (int)(rr.stdout_len < 200 ? rr.stdout_len : 200), rr.stdout_buf ? rr.stdout_buf : "",
            (int)(rr.stderr_len < 200 ? rr.stderr_len : 200), rr.stderr_buf ? rr.stderr_buf : "");
    hu_run_result_free(c->alloc, &rr);
    return rok;
}

#endif /* __APPLE__ && __MACH__ && !HU_IS_TEST */

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

/* Safety net: runs the full output-validator chain (assistant_closer +
 * role_consistency + persona_narrator + …), then collapses double spaces and
 * trims whitespace. Modifies in-place. */
size_t imessage_sanitize_output(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;

    {
        hu_allocator_t local_alloc = hu_system_allocator();
        hu_output_validator_chain_t *out_chain = NULL;
        if (hu_validators_build_default_outbound_chain(&local_alloc, NULL, 0, &out_chain) ==
            HU_OK) {
            hu_chain_result_t cr;
            memset(&cr, 0, sizeof(cr));
            if (hu_output_validator_chain_execute(out_chain, &local_alloc, NULL, buf, len, &cr) ==
                HU_OK) {
                hu_observer_emit_validator_decision(NULL, &cr, NULL, len);
                if (cr.final_decision == HU_VALIDATOR_REJECT) {
                    /* Deny-by-default: clear buffer so no unsanitized content reaches iMessage. */
                    hu_log_warn("imessage", NULL,
                                "validator chain REJECT (via %s) — suppressing iMessage send",
                                cr.deciding_validator_name ? cr.deciding_validator_name
                                                           : "unknown");
                    buf[0] = '\0';
                    len = 0;
                } else if (cr.final_text) {
                    if (cr.final_text != buf && cr.final_text_len <= len) {
                        memcpy(buf, cr.final_text, cr.final_text_len);
                        len = cr.final_text_len;
                        buf[len] = '\0';
                    }
                }
                hu_chain_result_free(&local_alloc, &cr);
            }
            hu_output_validator_chain_destroy(out_chain);
        }
    }

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

const char *hu_imessage_reaction_to_tapback_name(hu_reaction_type_t reaction) {
    switch (reaction) {
    case HU_REACTION_HEART:
        return "love";
    case HU_REACTION_THUMBS_UP:
        return "like";
    case HU_REACTION_THUMBS_DOWN:
        return "dislike";
    case HU_REACTION_HAHA:
        return "laugh";
    case HU_REACTION_EMPHASIS:
        return "emphasize";
    case HU_REACTION_QUESTION:
        return "question";
    case HU_REACTION_CUSTOM_EMOJI:
        return "emoji";
    default:
        return NULL;
    }
}

const char *hu_imessage_effect_name(const char *style_id) {
    if (!style_id || !style_id[0])
        return NULL;
    if (strstr(style_id, "impact"))
        return "Slam";
    if (strstr(style_id, "loud"))
        return "Loud";
    if (strstr(style_id, "gentle"))
        return "Gentle";
    if (strstr(style_id, "invisibleink"))
        return "Invisible Ink";
    if (strstr(style_id, "CKConfettiEffect"))
        return "Confetti";
    if (strstr(style_id, "CKEchoEffect"))
        return "Echo";
    if (strstr(style_id, "CKFireworksEffect"))
        return "Fireworks";
    if (strstr(style_id, "CKHappyBirthdayEffect"))
        return "Happy Birthday";
    if (strstr(style_id, "CKHeartEffect"))
        return "Heart";
    if (strstr(style_id, "CKLasersEffect"))
        return "Lasers";
    if (strstr(style_id, "CKShootingStarEffect"))
        return "Shooting Star";
    if (strstr(style_id, "CKSparklesEffect"))
        return "Sparkles";
    if (strstr(style_id, "CKSpotlightEffect"))
        return "Spotlight";
    return NULL;
}

const char *hu_imessage_balloon_label(const char *balloon_id) {
    if (!balloon_id || !balloon_id[0])
        return NULL;
    /* Animoji/Memoji checked first: their bundle IDs often contain "Stickers" */
    if (strstr(balloon_id, "Animoji") || strstr(balloon_id, "animoji") ||
        strstr(balloon_id, "Memoji") || strstr(balloon_id, "memoji"))
        return "[Memoji]";
    if (strstr(balloon_id, "Sticker") || strstr(balloon_id, "sticker"))
        return "[Sticker]";
    return "[iMessage App]";
}

bool hu_imessage_text_is_placeholder(const char *text) {
    if (!text || text[0] == '\0')
        return true;
    return strcmp(text, "[Photo]") == 0 || strcmp(text, "[Video]") == 0 ||
           strcmp(text, "[Voice Message]") == 0;
}

size_t hu_imessage_copy_bounded(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    if (!dst || dst_cap == 0)
        return 0;
    if (!src || src_len == 0) {
        dst[0] = '\0';
        return 0;
    }
    size_t cplen = src_len >= dst_cap ? dst_cap - 1 : src_len;
    memcpy(dst, src, cplen);
    dst[cplen] = '\0';
    return cplen;
}

#if defined(HU_IMESSAGE_TAPBACK_ENABLED)
/*
 * Tapback mapping: hu_reaction_type_t -> iMessage AX context menu label.
 * chat.db associated_message_type values (for reference when reading tapbacks):
 *   2000=love, 2001=like, 2002=dislike, 2003=laugh, 2004=emphasis, 2005=question.
 * Sending uses JXA + System Events AXShowMenu; AppleScript has no native tapback API.
 * Requires accessibility permissions; UI hierarchy may vary by macOS version.
 */
static const char *imessage_reaction_to_ax_action_prefix(hu_reaction_type_t reaction) {
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

/* Build an AppleScript to send a POSIX file attachment via iMessage.
 * Pure string builder — does NOT execute the script.
 * Returns bytes written (excluding NUL) or 0 on error. */
size_t imessage_build_attach_script(char *out, size_t out_cap, const char *target_escaped,
                                    const char *path_escaped) {
    if (!out || out_cap < 128 || !target_escaped || !path_escaped)
        return 0;
    int n = snprintf(out, out_cap,
                     "tell application \"Messages\"\n"
                     "  set targetService to 1st service whose service type = iMessage\n"
                     "  set targetBuddy to buddy \"%s\" of targetService\n"
                     "  send POSIX file \"%s\" to targetBuddy\n"
                     "end tell",
                     target_escaped, path_escaped);
    if (n > 0 && (size_t)n < out_cap)
        return (size_t)n;
    return 0;
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

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
/*
 * Typing indicator with chat ID caching and group chat support.
 * Caches target to skip expensive chat iteration on repeat sends.
 * Skipped when the daemon already called start_typing (typing_active).
 */
static void imessage_simulate_typing(hu_imessage_ctx_t *c, const char *tgt, size_t tgt_len,
                                     size_t message_len) {
    if (!c || atomic_load(&c->typing_active))
        return;

    unsigned int delay_ms =
        hu_imessage_typing_duration(message_len, (uint32_t)time(NULL) ^ (uint32_t)message_len);

    size_t tgt_esc_cap = tgt_len * 2 + 1;
    if (tgt_esc_cap > 4096)
        return;

    char tgt_esc[4096];
    escape_for_applescript(tgt_esc, sizeof(tgt_esc), tgt, tgt_len);

    if (delay_ms <= 3000) {
        bool same_target = (c->typing_last_target_len == tgt_len && tgt_len > 0 &&
                            memcmp(c->typing_last_target, tgt, tgt_len) == 0);
        if (tgt_len > 0 && tgt_len < sizeof(c->typing_last_target)) {
            memcpy(c->typing_last_target, tgt, tgt_len);
            c->typing_last_target[tgt_len] = '\0';
            c->typing_last_target_len = tgt_len;
        }

        char typing_script[1024];
        int ts_n;
        if (same_target) {
            ts_n = snprintf(typing_script, sizeof(typing_script),
                            "tell application \"Messages\" to activate\n"
                            "delay 0.2\n"
                            "tell application \"System Events\" to tell process \"Messages\"\n"
                            "  keystroke \".\"\n"
                            "  delay %.1f\n"
                            "  keystroke \"a\" using command down\n"
                            "  key code 51\n"
                            "end tell",
                            (float)delay_ms / 1000.0f);
        } else {
            ts_n = snprintf(typing_script, sizeof(typing_script),
                            "tell application \"Messages\"\n"
                            "  activate\n"
                            "  set targetHandle to \"%s\"\n"
                            "  set targetChat to missing value\n"
                            "  repeat with c in every chat\n"
                            "    try\n"
                            "      repeat with p in participants of c\n"
                            "        if handle of p is targetHandle then\n"
                            "          set targetChat to c\n"
                            "          exit repeat\n"
                            "        end if\n"
                            "      end repeat\n"
                            "    end try\n"
                            "    if targetChat is not missing value then exit repeat\n"
                            "  end repeat\n"
                            "end tell\n"
                            "delay 0.3\n"
                            "tell application \"System Events\" to tell process \"Messages\"\n"
                            "  keystroke \".\"\n"
                            "  delay %.1f\n"
                            "  keystroke \"a\" using command down\n"
                            "  key code 51\n"
                            "end tell",
                            tgt_esc, (float)delay_ms / 1000.0f);
        }
        if (ts_n > 0 && (size_t)ts_n < sizeof(typing_script)) {
            const char *ts_argv[] = {"osascript", "-e", typing_script, NULL};
            hu_run_result_t ts_result = {0};
            hu_error_t ts_err = hu_process_run(c->alloc, ts_argv, NULL, 65536, &ts_result);
            hu_run_result_free(c->alloc, &ts_result);
            if (ts_err != HU_OK && getenv("HU_DEBUG"))
                hu_log_error("imessage", NULL, "typing indicator failed (accessibility?)");
        }
    } else {
        /* Longer messages: re-trigger typing indicator every ~2.5s so the
         * bubble stays visible for the entire simulated composing period. */
        bool same_target = (c->typing_last_target_len == tgt_len && tgt_len > 0 &&
                            memcmp(c->typing_last_target, tgt, tgt_len) == 0);
        if (tgt_len > 0 && tgt_len < sizeof(c->typing_last_target)) {
            memcpy(c->typing_last_target, tgt, tgt_len);
            c->typing_last_target[tgt_len] = '\0';
            c->typing_last_target_len = tgt_len;
        }

        unsigned int remaining = delay_ms;
        while (remaining > 0) {
            unsigned int chunk = remaining > 2500 ? 2500 : remaining;
            char typing_script[1024];
            int ts_n;
            if (same_target) {
                ts_n = snprintf(typing_script, sizeof(typing_script),
                                "tell application \"Messages\" to activate\n"
                                "delay 0.2\n"
                                "tell application \"System Events\" to tell process "
                                "\"Messages\"\n"
                                "  keystroke \".\"\n"
                                "  delay %.1f\n"
                                "  keystroke \"a\" using command down\n"
                                "  key code 51\n"
                                "end tell",
                                (float)chunk / 1000.0f);
            } else {
                ts_n = snprintf(typing_script, sizeof(typing_script),
                                "tell application \"Messages\"\n"
                                "  activate\n"
                                "  set targetHandle to \"%s\"\n"
                                "  set targetChat to missing value\n"
                                "  repeat with c in every chat\n"
                                "    try\n"
                                "      repeat with p in participants of c\n"
                                "        if handle of p is targetHandle then\n"
                                "          set targetChat to c\n"
                                "          exit repeat\n"
                                "        end if\n"
                                "      end repeat\n"
                                "    end try\n"
                                "    if targetChat is not missing value then exit repeat\n"
                                "  end repeat\n"
                                "end tell\n"
                                "delay 0.3\n"
                                "tell application \"System Events\" to tell process "
                                "\"Messages\"\n"
                                "  keystroke \".\"\n"
                                "  delay %.1f\n"
                                "  keystroke \"a\" using command down\n"
                                "  key code 51\n"
                                "end tell",
                                tgt_esc, (float)chunk / 1000.0f);
                same_target = true;
            }
            if (ts_n > 0 && (size_t)ts_n < sizeof(typing_script)) {
                const char *ts_argv[] = {"osascript", "-e", typing_script, NULL};
                hu_run_result_t ts_result = {0};
                hu_error_t ts_err = hu_process_run(c->alloc, ts_argv, NULL, 65536, &ts_result);
                hu_run_result_free(c->alloc, &ts_result);
                if (ts_err != HU_OK) {
                    usleep((unsigned int)(remaining) * 1000);
                    break;
                }
            } else {
                usleep((unsigned int)(remaining) * 1000);
                break;
            }
            remaining -= chunk;
        }
    }
}
#endif

#endif

/* Platform-agnostic helper: called from the daemon's inbound-batch context
 * builder regardless of channel availability. Must live OUTSIDE the
 * (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST guard above so the
 * symbol is emitted on Linux production builds where neither macro is set.
 * hu_imessage_lookup_message_by_guid returns HU_ERR_NOT_SUPPORTED on platforms
 * without chat.db, in which case this helper returns 0 (no hint). */
size_t hu_imessage_build_inline_reply_hint_for_batch(hu_allocator_t *alloc,
                                                     const hu_channel_loop_msg_t *msgs,
                                                     size_t msg_count, char *out_buf,
                                                     size_t out_cap) {
    if (!alloc || !msgs || msg_count == 0 || !out_buf || out_cap == 0)
        return 0;

    const char *reply_guid = NULL;
    for (size_t i = 0; i < msg_count; i++) {
        if (msgs[i].reply_to_guid[0]) {
            reply_guid = msgs[i].reply_to_guid;
            break;
        }
    }
    if (!reply_guid)
        return 0;

    char orig_text[512];
    size_t orig_len = 0;
    hu_error_t lr_err = hu_imessage_lookup_message_by_guid(alloc, reply_guid, strlen(reply_guid),
                                                           orig_text, sizeof(orig_text), &orig_len);
    if (lr_err != HU_OK || orig_len == 0)
        return 0;

    return hu_conversation_build_inline_reply_hint(orig_text, orig_len, out_buf, out_cap);
}

static hu_error_t imessage_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
#if HU_IS_TEST
    (void)target;
    (void)target_len;
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
        if (!c)
            return HU_ERR_INVALID_ARGUMENT;
        /* Match production imessage_send validation (no AppleScript in tests). */
        if (message_len > 0 && !message)
            return HU_ERR_INVALID_ARGUMENT;
        if (message_len == 0 && media_count == 0)
            return HU_ERR_INVALID_ARGUMENT;
        c->last_media_count = media_count;
        if (media && media_count > 0 && media[0]) {
            size_t mp_len = strlen(media[0]);
            if (mp_len > sizeof(c->last_media_path) - 1)
                mp_len = sizeof(c->last_media_path) - 1;
            memcpy(c->last_media_path, media[0], mp_len);
            c->last_media_path[mp_len] = '\0';
        } else {
            c->last_media_path[0] = '\0';
        }
        /* Persona overlay rendering: semantic transform applied before any
         * iMessage-specific presentation cleanup. Test path captures the
         * overlay-rendered text so per-channel divergence is observable. */
        char *rendered = NULL;
        size_t rendered_len = 0;
        if (message_len > 0) {
            const hu_persona_overlay_t *ov = NULL;
            const char *contact_warmth = NULL;
            if (c->persona) {
                ov = hu_persona_find_overlay(c->persona, "imessage", 8);
                if (target && target_len > 0) {
                    const hu_contact_profile_t *cp =
                        hu_persona_find_contact(c->persona, target, target_len);
                    if (cp && cp->warmth_level)
                        contact_warmth = cp->warmth_level;
                }
            }
            hu_error_t rerr = hu_persona_render_for_channel_with_warmth(
                ov, contact_warmth, message, message_len, c->alloc, &rendered, &rendered_len);
            if (rerr != HU_OK)
                return rerr;
            message = rendered;
            message_len = rendered_len;
        }
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        else
            len = 0;
        c->last_message[len] = '\0';
        c->last_message_len = len;
        if (rendered)
            c->alloc->free(c->alloc->ctx, rendered, rendered_len + 1);
        return HU_OK;
    }
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    /* Use target if provided, else default_target */
    const char *tgt = target;
    size_t tgt_len = target_len;
    if ((!tgt || tgt_len == 0) && c->default_target && c->default_target_len > 0) {
        tgt = c->default_target;
        tgt_len = c->default_target_len;
    }
    if (!c || !c->alloc || !tgt || tgt_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len == 0 && media_count == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len > 0 && !message)
        return HU_ERR_INVALID_ARGUMENT;

    /* T0.1 BLUE GUARD (docs/plans/2026-07-19-native-imessage).
     * Never emit a green bubble. chat.db is the SIP-free source of truth for
     * which service a handle actually routes over; RCS counts as green. When
     * the evidence says not-iMessage (or says nothing at all), HOLD — staying
     * silent beats texting as SMS from a persona that only ever texts blue.
     * Override for deliberate SMS use: HU_IMESSAGE_ALLOW_GREEN=1. */
    if (!getenv("HU_IMESSAGE_ALLOW_GREEN")) {
        hu_imessage_service_t recent = HU_IMSG_SERVICE_UNKNOWN;
        hu_imessage_service_t handle_svc = HU_IMSG_SERVICE_UNKNOWN;
        imsg_lookup_services_for_handle(tgt, tgt_len, &recent, &handle_svc);
        /* T0.1b: chat.db only says how this handle routed in the PAST. When the
         * IMCore bridge is live, ask Apple the current question directly and
         * let that answer win; an unavailable/failed lookup returns
         * INDETERMINATE and falls back to the chat.db inference above. */
        hu_whois_reach_t live = HU_WHOIS_INDETERMINATE;
        const hu_imessage_caps_t *caps = imsg_caps_cached(c);
        if (caps && caps->advanced)
            live = hu_imessage_whois_probe_cached(c->alloc, tgt, tgt_len);
        const bool whois_negative_binds = getenv("HU_IMESSAGE_WHOIS_STRICT") != NULL;
        if (hu_imessage_blue_verdict_live(live, recent, handle_svc, whois_negative_binds) ==
            HU_BLUE_HOLD) {
            hu_log_warn(
                "imessage", NULL,
                "blue_guard: HELD send to %.*s — not iMessage-reachable "
                "(whois=%d recent=%d handle=%d); set HU_IMESSAGE_ALLOW_GREEN=1 to permit SMS",
                (int)(tgt_len > 24 ? 24 : tgt_len), tgt, (int)live, (int)recent, (int)handle_svc);
            return HU_ERR_NOT_SUPPORTED;
        }
    }

    /* Persona overlay rendering: applied BEFORE iMessage's markdown strip
     * and AI-phrase sanitization. The overlay is a semantic transform
     * (formality, length, emoji policy); the cleanup that follows is
     * iMessage-specific presentation. Doing overlay first means the
     * sanitizer never sees text the overlay would have removed, and
     * a NULL overlay yields an identity copy so the cleanup paths are
     * unchanged (AC-6). */
    char *overlay_buf = NULL;
    size_t overlay_buf_len = 0;
    if (message_len > 0) {
        const hu_persona_overlay_t *ov = NULL;
        const char *contact_warmth = NULL;
        if (c->persona) {
            ov = hu_persona_find_overlay(c->persona, "imessage", 8);
            const hu_contact_profile_t *cp = hu_persona_find_contact(c->persona, tgt, tgt_len);
            if (cp && cp->warmth_level)
                contact_warmth = cp->warmth_level;
        }
        hu_error_t rerr = hu_persona_render_for_channel_with_warmth(
            ov, contact_warmth, message, message_len, c->alloc, &overlay_buf, &overlay_buf_len);
        if (rerr != HU_OK)
            return rerr;
        message = overlay_buf;
        message_len = overlay_buf_len;
    }

    hu_error_t send_err = HU_OK;
    char *clean = NULL;
    size_t clean_cap = 0;

    /* Skip empty text send when we have media (voice-only) */
    if (message_len > 0) {
        /*
         * Post-processing: strip markdown and sanitize AI-sounding phrases
         * before sending via iMessage. Works in-place on a mutable copy.
         */
        clean_cap = message_len + 1;
        clean = (char *)c->alloc->alloc(c->alloc->ctx, clean_cap);
        if (!clean) {
            if (overlay_buf)
                c->alloc->free(c->alloc->ctx, overlay_buf, overlay_buf_len + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }
        {
            size_t out_i = 0;
            size_t i = 0;
            while (i < message_len) {
                if (message[i] == '*') {
                    while (i < message_len && message[i] == '*')
                        i++;
                    continue;
                }
                if ((i == 0 || message[i - 1] == '\n') && message[i] == '#') {
                    while (i < message_len && message[i] == '#')
                        i++;
                    if (i < message_len && message[i] == ' ')
                        i++;
                    continue;
                }
                if ((i == 0 || message[i - 1] == '\n') && i + 1 < message_len &&
                    (message[i] == '-' || message[i] == '*') && message[i + 1] == ' ') {
                    i += 2;
                    continue;
                }
                if (message[i] == '`') {
                    i++;
                    continue;
                }
                clean[out_i++] = message[i];
                i++;
            }
            clean[out_i] = '\0';
            message = clean;
            message_len = out_i;
        }

        message_len = imessage_sanitize_output(clean, message_len);

        /* Hard length cap for iMessage: truncate at sentence boundary near 1000 chars */
        if (message_len > 1000) {
            size_t cut = 1000;
            while (cut > 200 && clean[cut] != '.' && clean[cut] != '!' && clean[cut] != '?')
                cut--;
            if (cut > 200) {
                message_len = cut + 1;
                clean[message_len] = '\0';
            } else {
                size_t space_cut = 1000;
                while (space_cut > 200 && clean[space_cut] != ' ')
                    space_cut--;
                if (space_cut > 200)
                    message_len = space_cut;
                else
                    message_len = 1000;
                clean[message_len] = '\0';
            }
        }

        imessage_simulate_typing(c, tgt, tgt_len, message_len);

        {
            if (c->use_imsg_cli && imsg_cli_available(c)) {
                char tgt_buf[256];
                size_t tb = tgt_len < sizeof(tgt_buf) - 1 ? tgt_len : sizeof(tgt_buf) - 1;
                memcpy(tgt_buf, tgt, tb);
                tgt_buf[tb] = '\0';
                const char *imsg_argv[] = {"imsg",  "send",      "--to",     tgt_buf, "--text",
                                           message, "--service", "imessage", NULL};
                hu_run_result_t imsg_result = {0};
                hu_error_t imsg_err =
                    hu_process_run_with_timeout(c->alloc, imsg_argv, NULL, 65536, 15, &imsg_result);
                bool imsg_ok =
                    (imsg_err == HU_OK && imsg_result.success && imsg_result.exit_code == 0);
                hu_run_result_free(c->alloc, &imsg_result);
                if (imsg_ok) {
                    imessage_record_sent(c, message, message_len);
                    goto imsg_media;
                }
                if (getenv("HU_DEBUG"))
                    hu_log_info("imessage", NULL, "imsg send failed, falling back to AppleScript");
            }
        }
        /* Escaped strings: worst case 2x length */
        size_t msg_esc_cap = message_len * 2 + 1;
        size_t tgt_esc_cap = tgt_len * 2 + 1;
        if (msg_esc_cap > 65536 || tgt_esc_cap > 4096) {
            send_err = HU_ERR_INVALID_ARGUMENT;
            goto imsg_cleanup;
        }

        char *msg_esc = (char *)c->alloc->alloc(c->alloc->ctx, msg_esc_cap);
        char *tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, tgt_esc_cap);
        if (!msg_esc || !tgt_esc) {
            if (msg_esc)
                c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
            if (tgt_esc)
                c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
            send_err = HU_ERR_OUT_OF_MEMORY;
            goto imsg_cleanup;
        }
        escape_for_applescript(msg_esc, msg_esc_cap, message, message_len);
        escape_for_applescript(tgt_esc, tgt_esc_cap, tgt, tgt_len);

        /* Target the iMessage service explicitly for reliability on modern macOS */
        size_t script_cap = 256 + strlen(msg_esc) + strlen(tgt_esc);
        char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
        if (!script) {
            c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
            c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
            send_err = HU_ERR_OUT_OF_MEMORY;
            goto imsg_cleanup;
        }
        int n = snprintf(script, script_cap,
                         "tell application \"Messages\"\n"
                         "  set targetService to 1st service whose service type = iMessage\n"
                         "  set targetBuddy to buddy \"%s\" of targetService\n"
                         "  send \"%s\" to targetBuddy\n"
                         "end tell",
                         tgt_esc, msg_esc);

        c->alloc->free(c->alloc->ctx, msg_esc, msg_esc_cap);
        c->alloc->free(c->alloc->ctx, tgt_esc, tgt_esc_cap);
        if (n < 0 || (size_t)n >= script_cap) {
            c->alloc->free(c->alloc->ctx, script, script_cap);
            send_err = HU_ERR_INTERNAL;
            goto imsg_cleanup;
        }

        {
#ifdef HU_IS_TEST
            if (g_imessage_test_send_stub != NULL) {
                /* Test stub: invoke callback instead of osascript */
                (*g_imessage_test_send_stub)(tgt, tgt_len, message, message_len);
                c->alloc->free(c->alloc->ctx, script, script_cap);
                send_err = HU_OK;
            } else {
#endif
                const char *argv[] = {"osascript", "-e", script, NULL};
                hu_run_result_t result = {0};
                hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
                c->alloc->free(c->alloc->ctx, script, script_cap);
                bool ok = (err == HU_OK && result.success && result.exit_code == 0);
                hu_run_result_free(c->alloc, &result);
                if (err || !ok)
                    send_err = HU_ERR_CHANNEL_SEND;
#ifdef HU_IS_TEST
            }
#endif
        }

        if (send_err == HU_OK)
            imessage_record_sent(c, message, message_len);
    }

#if !HU_IS_TEST
imsg_media:
    /* Send media attachments (local file paths only) after text succeeds.
     * Prefer imsg send --file when available (faster, better error reporting);
     * fall back to AppleScript per-attachment on failure. */
    if (send_err == HU_OK && media && media_count > 0) {
        bool try_imsg_file = c->use_imsg_cli && imsg_cli_available(c);
        char imsg_tgt_buf[256];
        if (try_imsg_file) {
            size_t tb = tgt_len < sizeof(imsg_tgt_buf) - 1 ? tgt_len : sizeof(imsg_tgt_buf) - 1;
            memcpy(imsg_tgt_buf, tgt, tb);
            imsg_tgt_buf[tb] = '\0';
        }

        size_t m_tgt_cap = tgt_len * 2 + 1;
        char *m_tgt_esc = NULL;
        if (m_tgt_cap <= 4096) {
            m_tgt_esc = (char *)c->alloc->alloc(c->alloc->ctx, m_tgt_cap);
            if (m_tgt_esc)
                escape_for_applescript(m_tgt_esc, m_tgt_cap, tgt, tgt_len);
        }

        for (size_t i = 0; i < media_count && send_err == HU_OK; i++) {
            const char *url = media[i];
            if (!url || url[0] != '/')
                continue;
            if (access(url, R_OK) != 0)
                continue;

            if (try_imsg_file) {
                const char *fa[] = {"imsg", "send",      "--to",     imsg_tgt_buf, "--file",
                                    url,    "--service", "imessage", NULL};
                hu_run_result_t ir = {0};
                hu_error_t ie = hu_process_run_with_timeout(c->alloc, fa, NULL, 65536, 15, &ir);
                bool fok = (ie == HU_OK && ir.success && ir.exit_code == 0);
                hu_run_result_free(c->alloc, &ir);
                if (fok)
                    continue;
                if (getenv("HU_DEBUG"))
                    hu_log_info("imessage", NULL,
                                "imsg send --file failed, falling back to AppleScript");
            }

            if (!m_tgt_esc) {
                send_err = HU_ERR_CHANNEL_SEND;
                break;
            }
            size_t path_len = strlen(url);
            size_t path_esc_cap = path_len * 2 + 1;
            if (path_esc_cap > 8192)
                continue;
            char *path_esc = (char *)c->alloc->alloc(c->alloc->ctx, path_esc_cap);
            if (!path_esc)
                continue;
            escape_for_applescript(path_esc, path_esc_cap, url, path_len);
            size_t m_script_cap = 256 + strlen(m_tgt_esc) + strlen(path_esc);
            char *m_script = (char *)c->alloc->alloc(c->alloc->ctx, m_script_cap);
            if (m_script) {
                size_t m_n =
                    imessage_build_attach_script(m_script, m_script_cap, m_tgt_esc, path_esc);
                c->alloc->free(c->alloc->ctx, path_esc, path_esc_cap);
                if (m_n > 0) {
                    const char *argv[] = {"osascript", "-e", m_script, NULL};
                    hu_run_result_t result = {0};
                    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
                    bool ok = (err == HU_OK && result.success && result.exit_code == 0);
                    hu_run_result_free(c->alloc, &result);
                    if (!ok)
                        send_err = HU_ERR_CHANNEL_SEND;
                }
                c->alloc->free(c->alloc->ctx, m_script, m_script_cap);
            } else {
                c->alloc->free(c->alloc->ctx, path_esc, path_esc_cap);
            }
        }

        if (m_tgt_esc)
            c->alloc->free(c->alloc->ctx, m_tgt_esc, m_tgt_cap);
    }
#endif

imsg_cleanup:
    if (clean)
        c->alloc->free(c->alloc->ctx, clean, clean_cap);
    if (overlay_buf)
        c->alloc->free(c->alloc->ctx, overlay_buf, overlay_buf_len + 1);
    return send_err;
#endif
}

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

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
hu_error_t hu_imessage_build_tapback_context(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len, char **out, size_t *out_len) {
    if (!alloc || !contact_id || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char db_path[512];
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    const char *sql =
        "SELECT m.associated_message_type, COUNT(*) "
        "FROM message m "
        "WHERE m.is_from_me = 0 "
        "  AND m.associated_message_type BETWEEN 2000 AND 2006 "
        "  AND m.associated_message_guid IN ("
        "    SELECT m2.guid FROM message m2 "
        "    WHERE m2.is_from_me = 1 "
        "    AND m2.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
        "    ORDER BY m2.date DESC LIMIT 5"
        "  ) "
        "  AND m.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000 "
        "GROUP BY m.associated_message_type";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int hearts = 0, likes = 0, dislikes = 0, laughs = 0, emphasis = 0, questions = 0;
    int custom_emoji = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int type = sqlite3_column_int(stmt, 0);
        int cnt = sqlite3_column_int(stmt, 1);
        switch (type) {
        case 2000:
            hearts = cnt;
            break;
        case 2001:
            likes = cnt;
            break;
        case 2002:
            dislikes = cnt;
            break;
        case 2003:
            laughs = cnt;
            break;
        case 2004:
            emphasis = cnt;
            break;
        case 2005:
            questions = cnt;
            break;
        case 2006:
            custom_emoji = cnt;
            break;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    int total = hearts + likes + dislikes + laughs + emphasis + questions + custom_emoji;
    if (total == 0)
        return HU_OK;

    char buf[256];
    size_t pos = 0;
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "[REACTIONS on your recent messages:");
    if (hearts > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d heart%s", hearts, hearts > 1 ? "s" : "");
    if (likes > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d like%s", likes, likes > 1 ? "s" : "");
    if (laughs > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d laugh%s", laughs, laughs > 1 ? "s" : "");
    if (emphasis > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d emphasis", emphasis);
    if (questions > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d question%s", questions,
                             questions > 1 ? "s" : "");
    if (dislikes > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d dislike%s", dislikes,
                             dislikes > 1 ? "s" : "");
    if (custom_emoji > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " %d emoji reaction%s", custom_emoji,
                             custom_emoji > 1 ? "s" : "");
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "]");

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

int hu_imessage_count_recent_gif_tapbacks(const char *contact_id, size_t contact_id_len) {
    if (!contact_id || contact_id_len == 0)
        return 0;

    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return 0;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return 0;

    /* Find positive tapbacks (love/like/laugh/emphasis/emoji = 2000-2004,2006) on our
     * messages that have GIF attachments, from this contact, in the last 24 hours. */
    const char *sql =
        "SELECT COUNT(*) FROM message m "
        "WHERE m.is_from_me = 0 "
        "  AND (m.associated_message_type BETWEEN 2000 AND 2004 "
        "       OR m.associated_message_type = 2006) "
        "  AND m.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
        "  AND m.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000 "
        "  AND m.associated_message_guid IN ("
        "    SELECT m2.guid FROM message m2 "
        "    JOIN message_attachment_join maj ON maj.message_id = m2.ROWID "
        "    JOIN attachment a ON maj.attachment_id = a.ROWID "
        "    WHERE m2.is_from_me = 1 "
        "      AND LOWER(a.filename) LIKE '%.gif' "
        "      AND m2.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000"
        "  )";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

int hu_imessage_count_recent_music_tapbacks(const char *contact_id, size_t contact_id_len) {
    if (!contact_id || contact_id_len == 0)
        return 0;

    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return 0;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return 0;

    const char *sql =
        "SELECT COUNT(*) FROM message m "
        "WHERE m.is_from_me = 0 "
        "  AND (m.associated_message_type BETWEEN 2000 AND 2004 "
        "       OR m.associated_message_type = 2006) "
        "  AND m.handle_id = (SELECT ROWID FROM handle WHERE id = ?1) "
        "  AND m.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000 "
        "  AND m.associated_message_guid IN ("
        "    SELECT m2.guid FROM message m2 "
        "    JOIN message_attachment_join maj ON maj.message_id = m2.ROWID "
        "    JOIN attachment a ON maj.attachment_id = a.ROWID "
        "    WHERE m2.is_from_me = 1 "
        "      AND LOWER(a.filename) LIKE '%.m4a' "
        "      AND m2.date > (strftime('%s', 'now') - 86400) * 1000000000 - 978307200000000000"
        "  )";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return cnt;
}

int64_t hu_imessage_get_latest_sent_rowid(const char *handle, size_t handle_len) {
    if (!handle || handle_len == 0)
        return -1;

    const char *home = getenv("HOME");
    if (!home)
        return -1;

    char db_path[512];
    int dp = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (dp < 0 || (size_t)dp >= sizeof(db_path))
        return -1;

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return -1;

    const char *sql = "SELECT MAX(m.ROWID) FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE m.is_from_me = 1 AND h.id = ?1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    char hbuf[128];
    size_t hlen = handle_len < sizeof(hbuf) - 1 ? handle_len : sizeof(hbuf) - 1;
    memcpy(hbuf, handle, hlen);
    hbuf[hlen] = '\0';
    sqlite3_bind_text(stmt, 1, hbuf, (int)hlen, SQLITE_STATIC);

    int64_t rowid = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        rowid = sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rowid;
}

hu_error_t hu_imessage_build_read_receipt_context(hu_allocator_t *alloc, const char *contact_id,
                                                  size_t contact_id_len, char **out,
                                                  size_t *out_len) {
    if (!alloc || !contact_id || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char db_path[512];
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    /* Find our last sent message to this contact and check its read status */
    const char *sql = "SELECT m.date, m.date_delivered, m.date_read, m.text "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "ORDER BY m.date DESC LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    char buf[256];
    buf[0] = '\0';
    size_t pos = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t sent_date = sqlite3_column_int64(stmt, 0);
        int64_t delivered = sqlite3_column_int64(stmt, 1);
        int64_t read_date = sqlite3_column_int64(stmt, 2);

        /* Convert Apple epoch (nanoseconds since 2001-01-01) to Unix epoch */
        int64_t apple_epoch = 978307200LL;
        int64_t sent_unix = apple_epoch + sent_date / 1000000000LL;
        int64_t now_unix = (int64_t)time(NULL);
        int64_t age_seconds = now_unix - sent_unix;

        /* Also check: has there been a reply from them after our message? */
        sqlite3_stmt *reply_stmt = NULL;
        const char *reply_sql = "SELECT COUNT(*) FROM message m "
                                "JOIN handle h ON m.handle_id = h.ROWID "
                                "WHERE h.id = ?1 AND m.is_from_me = 0 AND m.date > ?2 "
                                "AND m.associated_message_type = 0";
        bool has_reply = false;
        if (sqlite3_prepare_v2(db, reply_sql, -1, &reply_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(reply_stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);
            sqlite3_bind_int64(reply_stmt, 2, sent_date);
            if (sqlite3_step(reply_stmt) == SQLITE_ROW)
                has_reply = sqlite3_column_int(reply_stmt, 0) > 0;
            sqlite3_finalize(reply_stmt);
        }

        if (!has_reply && read_date > 0 && age_seconds > 300 && age_seconds < 86400) {
            /* Read but no reply — they saw it but haven't responded */
            int64_t read_unix = apple_epoch + read_date / 1000000000LL;
            int64_t since_read = now_unix - read_unix;
            if (since_read > 60) {
                int mins = (int)(since_read / 60);
                if (mins > 60) {
                    pos = (size_t)snprintf(
                        buf, sizeof(buf),
                        "[READ RECEIPT: They read your last message %dh ago but haven't replied. "
                        "Don't mention this — just be natural, don't guilt-trip.]",
                        mins / 60);
                } else {
                    pos =
                        (size_t)snprintf(buf, sizeof(buf),
                                         "[READ RECEIPT: They read your last message %d min ago "
                                         "but haven't replied. "
                                         "Don't mention this — just be natural, don't guilt-trip.]",
                                         mins);
                }
            }
        } else if (!has_reply && delivered > 0 && read_date == 0 && age_seconds > 600) {
            /* Delivered but not read */
            pos = (size_t)snprintf(buf, sizeof(buf),
                                   "[READ RECEIPT: Your last message was delivered but not yet "
                                   "read. They may be busy.]");
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (pos > 0 && pos < sizeof(buf)) {
        *out = hu_strndup(alloc, buf, pos);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = pos;
    }
    return HU_OK;
}
#else
hu_error_t hu_imessage_build_tapback_context(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len, char **out, size_t *out_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK;
}

int hu_imessage_count_recent_gif_tapbacks(const char *contact_id, size_t contact_id_len) {
    (void)contact_id;
    (void)contact_id_len;
    return 0;
}

int hu_imessage_count_recent_music_tapbacks(const char *contact_id, size_t contact_id_len) {
    (void)contact_id;
    (void)contact_id_len;
    return 0;
}

int64_t hu_imessage_get_latest_sent_rowid(const char *handle, size_t handle_len) {
    (void)handle;
    (void)handle_len;
    return -1;
}

hu_error_t hu_imessage_build_read_receipt_context(hu_allocator_t *alloc, const char *contact_id,
                                                  size_t contact_id_len, char **out,
                                                  size_t *out_len) {
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_OK;
}
#endif

hu_error_t hu_imessage_find_unreplied_read(const char *contact_id, size_t contact_id_len,
                                           int64_t *out_msg_id, uint64_t *out_read_at_ms) {
    if (out_msg_id)
        *out_msg_id = 0;
    if (out_read_at_ms)
        *out_read_at_ms = 0;
    if (!contact_id || contact_id_len == 0 || !out_msg_id || !out_read_at_ms)
        return HU_ERR_INVALID_ARGUMENT;

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
    /* Find the most recent outbound to `contact_id`. Mirrors the structure
     * of hu_imessage_build_read_receipt_context but returns structured data
     * instead of an LLM prompt string. */
    char db_path[512];
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    const char *sql = "SELECT m.ROWID, m.date, m.date_read "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 "
                      "ORDER BY m.date DESC LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int64_t msg_id = 0;
    int64_t sent_date = 0;
    int64_t read_date = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        msg_id = sqlite3_column_int64(stmt, 0);
        sent_date = sqlite3_column_int64(stmt, 1);
        read_date = sqlite3_column_int64(stmt, 2);
    }
    sqlite3_finalize(stmt);

    if (msg_id == 0 || read_date <= 0) {
        /* No outbound, or it hasn't been read yet — no follow-up to schedule. */
        sqlite3_close(db);
        return HU_OK;
    }

    /* Check for an inbound reply since the outbound. */
    bool has_reply = false;
    sqlite3_stmt *reply_stmt = NULL;
    const char *reply_sql = "SELECT COUNT(*) FROM message m "
                            "JOIN handle h ON m.handle_id = h.ROWID "
                            "WHERE h.id = ?1 AND m.is_from_me = 0 AND m.date > ?2 "
                            "AND m.associated_message_type = 0";
    if (sqlite3_prepare_v2(db, reply_sql, -1, &reply_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(reply_stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);
        sqlite3_bind_int64(reply_stmt, 2, sent_date);
        if (sqlite3_step(reply_stmt) == SQLITE_ROW)
            has_reply = sqlite3_column_int(reply_stmt, 0) > 0;
        sqlite3_finalize(reply_stmt);
    }
    sqlite3_close(db);

    if (has_reply)
        return HU_OK;

    /* Convert Apple's date_read (nanoseconds since 2001-01-01) to wall-clock ms. */
    int64_t apple_epoch = 978307200LL;
    int64_t read_unix_sec = apple_epoch + read_date / 1000000000LL;
    *out_msg_id = msg_id;
    *out_read_at_ms = (uint64_t)read_unix_sec * 1000ULL;
    return HU_OK;
#else
    (void)contact_id_len;
    return HU_OK; /* test / non-Apple / no-SQLite: no result */
#endif
}

/* NEW FUNCTION FOR US-48-3: Find inbound unreplied message.
 *
 * Detects when the user (seth) has been unresponsive to an inbound message
 * from a contact. Queries for:
 * 1. Most recent inbound (is_from_me=0) from contact_id
 * 2. Verifies no outbound (is_from_me=1) exists after it
 * If both conditions hold, returns the message ID and timestamp. Otherwise
 * returns HU_OK with out_msg_id=0 (no unreplied inbound found). */
hu_error_t hu_imessage_find_inbound_unreplied(const char *contact_id, size_t contact_id_len,
                                              int64_t *out_msg_id, uint64_t *out_read_at_ms) {
    if (out_msg_id)
        *out_msg_id = 0;
    if (out_read_at_ms)
        *out_read_at_ms = 0;
    if (!contact_id || contact_id_len == 0 || !out_msg_id || !out_read_at_ms)
        return HU_ERR_INVALID_ARGUMENT;

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
    /* Open chat.db and find most recent inbound from contact. */
    char db_path[512];
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_IO;
    snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_IO;

    /* Query most recent INBOUND (is_from_me=0) from this contact. */
    const char *sql = "SELECT m.ROWID, m.date "
                      "FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 0 "
                      "ORDER BY m.date DESC LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';
    sqlite3_bind_text(stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);

    int64_t msg_id = 0;
    int64_t inbound_date = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        msg_id = sqlite3_column_int64(stmt, 0);
        inbound_date = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);

    if (msg_id == 0) {
        /* No inbound from this contact. */
        sqlite3_close(db);
        return HU_OK;
    }

    /* Check for any OUTBOUND (is_from_me=1) AFTER the inbound message.
     * If an outbound exists, the user has already replied. */
    bool has_reply = false;
    sqlite3_stmt *reply_stmt = NULL;
    const char *reply_sql = "SELECT COUNT(*) FROM message m "
                            "JOIN handle h ON m.handle_id = h.ROWID "
                            "WHERE h.id = ?1 AND m.is_from_me = 1 AND m.date > ?2";
    if (sqlite3_prepare_v2(db, reply_sql, -1, &reply_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(reply_stmt, 1, contact_buf, (int)clen, SQLITE_STATIC);
        sqlite3_bind_int64(reply_stmt, 2, inbound_date);
        if (sqlite3_step(reply_stmt) == SQLITE_ROW)
            has_reply = sqlite3_column_int(reply_stmt, 0) > 0;
        sqlite3_finalize(reply_stmt);
    }
    sqlite3_close(db);

    if (has_reply)
        return HU_OK; /* User has already replied; no follow-up needed. */

    /* Convert Apple's date (nanoseconds since 2001-01-01) to wall-clock ms. */
    int64_t apple_epoch = 978307200LL;
    int64_t inbound_unix_sec = apple_epoch + inbound_date / 1000000000LL;
    *out_msg_id = msg_id;
    *out_read_at_ms = (uint64_t)inbound_unix_sec * 1000ULL;
    return HU_OK;
#else
    (void)contact_id_len;
    return HU_OK; /* test / non-Apple / no-SQLite: no result */
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

static hu_error_t imessage_react(void *ctx, const char *target, size_t target_len,
                                 int64_t message_id, hu_reaction_type_t reaction) {
    (void)target;
    (void)target_len;
    (void)message_id;
#if HU_IS_TEST
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->last_reaction = reaction;
    c->last_reaction_message_id = message_id;
    return HU_OK;
#else
#if !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)reaction;
    return HU_ERR_NOT_SUPPORTED;
#elif !defined(HU_IMESSAGE_TAPBACK_ENABLED)
    {
        hu_imessage_ctx_t *c_try = (hu_imessage_ctx_t *)ctx;
        if (c_try && c_try->use_imsg_cli && imsg_cli_available(c_try) &&
            imsg_try_react(c_try, message_id, reaction))
            return HU_OK;
        (void)target;
        (void)target_len;
        if (getenv("HU_DEBUG"))
            hu_log_info("imessage", NULL,
                        "tapback: no imsg CLI and JXA disabled "
                        "(HU_IMESSAGE_TAPBACK_ENABLED=OFF)");
        return HU_ERR_NOT_SUPPORTED;
    }
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    const char *tapback_ax = imessage_reaction_to_ax_action_prefix(reaction);
    if (!tapback_ax)
        return HU_ERR_INVALID_ARGUMENT;
    if (!target || target_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (c->use_imsg_cli && imsg_cli_available(c) && imsg_try_react(c, message_id, reaction))
        return HU_OK;

    /* Tier 2: Native AX tapback — uses our process's Accessibility permission
     * directly, bypasses System Events keystroke restriction. */
    /* Fetch raw message text for AX menu-item matching. Intentionally does NOT
     * apply balloon_label or effect_name — tapback needs the original text that
     * the Messages UI displays, not decorated agent-facing labels. */
    char content_buf[256];
    size_t content_len = 0;
    int row_offset = -1; /* messages after target in same chat; -1 = unknown */
#if defined(HU_ENABLE_SQLITE)
    if (message_id > 0) {
        const char *home_env = getenv("HOME");
        if (home_env) {
            char db_path[512];
            int dn = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home_env);
            if (dn > 0 && (size_t)dn < sizeof(db_path)) {
                sqlite3 *db = NULL;
                if (imessage_open_chatdb(db_path, &db) == SQLITE_OK) {
                    sqlite3_stmt *stmt = NULL;
                    if (sqlite3_prepare_v2(
                            db, "SELECT text, attributedBody FROM message WHERE ROWID = ?", -1,
                            &stmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(stmt, 1, message_id);
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char *text = (const char *)sqlite3_column_text(stmt, 0);
                            if (!text || text[0] == '\0') {
                                const unsigned char *ab = sqlite3_column_blob(stmt, 1);
                                int ab_len = sqlite3_column_bytes(stmt, 1);
                                if (ab && ab_len > 0) {
                                    content_len = hu_imessage_extract_attributed_body(
                                        ab, (size_t)ab_len, content_buf, sizeof(content_buf));
                                }
                            } else {
                                size_t len = strlen(text);
                                if (len >= sizeof(content_buf))
                                    len = sizeof(content_buf) - 1;
                                memcpy(content_buf, text, len);
                                content_buf[len] = '\0';
                                content_len = len;
                            }
                        }
                        sqlite3_finalize(stmt);
                    }
                    /* Row offset: count non-tapback messages after this one in the same chat.
                     * Gives us a reliable index from the bottom of the transcript view. */
                    sqlite3_stmt *off_stmt = NULL;
                    const char *off_sql = "SELECT COUNT(*) FROM message m "
                                          "JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
                                          "WHERE cmj.chat_id = ("
                                          "  SELECT cmj2.chat_id FROM chat_message_join cmj2 "
                                          "  WHERE cmj2.message_id = ?1 LIMIT 1"
                                          ") AND m.ROWID > ?1 AND m.associated_message_type = 0";
                    if (sqlite3_prepare_v2(db, off_sql, -1, &off_stmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(off_stmt, 1, message_id);
                        if (sqlite3_step(off_stmt) == SQLITE_ROW)
                            row_offset = sqlite3_column_int(off_stmt, 0);
                        sqlite3_finalize(off_stmt);
                    }
                    sqlite3_close(db);
                }
            }
        }
    }
#endif
    (void)message_id;

    /* Try native AX tapback first (no subprocess, no keystroke restriction).
     * Must open the conversation in Messages so it's in the AX tree. */
    if (target && target_len > 0)
        ax_open_conversation(target, target_len);
    if (ax_tapback(content_len > 0 ? content_buf : NULL, row_offset, tapback_ax)) {
        hu_log_info("imessage", NULL, "tapback sent via native AX");
        return HU_OK;
    }

    /* Fall back to JXA subprocess (legacy, may fail on Sequoia+). */
    /* Escape content_prefix and tapback_ax for JavaScript string literals. */
    size_t esc_cap = (content_len + strlen(tapback_ax)) * 2 + 64;
    if (esc_cap > 2048)
        esc_cap = 2048;
    char *content_esc = (char *)c->alloc->alloc(c->alloc->ctx, esc_cap);
    if (!content_esc)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < content_len && j + 2 < esc_cap; i++) {
        if (content_buf[i] == '\\' || content_buf[i] == '"') {
            content_esc[j++] = '\\';
            content_esc[j++] = content_buf[i];
        } else if (content_buf[i] == '\n') {
            content_esc[j++] = '\\';
            content_esc[j++] = 'n';
        } else {
            content_esc[j++] = content_buf[i];
        }
    }
    content_esc[j] = '\0';
    size_t content_esc_len = j;

    size_t tapback_esc_cap = strlen(tapback_ax) * 2 + 16;
    char *tapback_esc = (char *)c->alloc->alloc(c->alloc->ctx, tapback_esc_cap);
    if (!tapback_esc) {
        c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    j = 0;
    for (size_t i = 0; tapback_ax[i] && j + 2 < tapback_esc_cap; i++) {
        if (tapback_ax[i] == '\\' || tapback_ax[i] == '"') {
            tapback_esc[j++] = '\\';
            tapback_esc[j++] = tapback_ax[i];
        } else {
            tapback_esc[j++] = tapback_ax[i];
        }
    }
    tapback_esc[j] = '\0';

    /*
     * JXA script: activate Messages, find row by offset or content, AXShowMenu, select tapback.
     * - rowOffset: messages after target in the transcript (-1 = unknown, use content match)
     * - contentPrefix: substring to match in table row (empty = use last row)
     * - tapbackAx: AX menu label (Love, Like, Ha Ha, etc.)
     * Requires accessibility permissions; UI hierarchy varies by macOS version.
     */
    static const char *script_tpl =
        "ObjC.import(\"stdlib\");"
        "var rowOff=%d;var contentPrefix=\"%s\";var tapbackAx=\"%s\";"
        "try{"
        "var M=Application(\"Messages\");M.activate();delay(0.5);"
        "var SE=Application(\"System Events\");var p=SE.processes[\"Messages\"];"
        "if(!p||!p.exists()){$.exit(1);}"
        "var w=p.windows();if(!w||w.length===0){$.exit(1);}"
        "var win=w[0];var t=win.tables();"
        "if(!t||t.length===0){$.exit(1);}"
        "var rows=t[t.length-1].rows();"
        "if(!rows||rows.length===0){$.exit(1);}"
        "var r=null;"
        /* Try row-offset first (most reliable when available). */
        "if(rowOff>=0&&rowOff<rows.length){"
        "var idx=rows.length-1-rowOff;"
        "var cand=rows[idx];var cv=\"\";"
        "try{if(cand.attributes&&cand.attributes.AXValue!==undefined)"
        "{cv=String(cand.attributes.AXValue);}}catch(e){}"
        "if(!contentPrefix||contentPrefix.length===0||"
        "(cv&&cv.indexOf(contentPrefix)!==-1)){r=cand;}"
        "}"
        /* Fall back to content-prefix scan from bottom. */
        "if(!r&&contentPrefix&&contentPrefix.length>0){"
        "for(var i=rows.length-1;i>=0;i--){"
        "var row=rows[i];var val=\"\";"
        "try{if(row.attributes&&row.attributes.AXValue!==undefined){val=String(row.attributes."
        "AXValue);}}catch(e){}"
        "try{if(!val&&row.cells&&row.cells.length>0){var "
        "c0=row.cells[0];if(c0.attributes&&c0.attributes.AXValue!==undefined){val=String(c0."
        "attributes.AXValue);}}}catch(e){}"
        "if(val&&val.indexOf(contentPrefix)!==-1){r=row;break;}"
        "}}"
        /* Last resort: use the most recent row. */
        "if(!r){r=rows[rows.length-1];}"
        "if(r.actions&&r.actions.AXShowMenu){r.actions.AXShowMenu.perform();}"
        "delay(0.5);"
        "var ctxMenus=r.menus?r.menus():[];"
        "if(ctxMenus&&ctxMenus.length>0){"
        "var items=ctxMenus[0].menuItems?ctxMenus[0].menuItems():[];"
        "for(var ii=0;ii<items.length;ii++){"
        "var it=items[ii];var title=(it.title?it.title():\"\")+\"\";"
        "if(title.indexOf(\"Tapback\")!==-1){it.click();delay(0.3);"
        "var sub=it.menus?it.menus():[];"
        "if(sub&&sub.length>0){var subItems=sub[0].menuItems?sub[0].menuItems():[];"
        "for(var si=0;si<subItems.length;si++){var "
        "st=(subItems[si].title?subItems[si].title():\"\")+\"\";"
        "if(st===tapbackAx){subItems[si].click();break;}}"
        "}break;}"
        "}}"
        "}"
        "$.exit(0);"
        "}catch(e){$.exit(1);}";

    size_t script_cap = 2560 + content_esc_len + strlen(tapback_esc);
    char *script = (char *)c->alloc->alloc(c->alloc->ctx, script_cap);
    if (!script) {
        c->alloc->free(c->alloc->ctx, tapback_esc, tapback_esc_cap);
        c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(script, script_cap, script_tpl, row_offset, content_esc, tapback_esc);
    c->alloc->free(c->alloc->ctx, tapback_esc, tapback_esc_cap);
    c->alloc->free(c->alloc->ctx, content_esc, esc_cap);
    if (n < 0 || (size_t)n >= script_cap) {
        c->alloc->free(c->alloc->ctx, script, script_cap);
        return HU_ERR_INTERNAL;
    }

    const char *argv[] = {"osascript", "-l", "JavaScript", "-e", script, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_with_timeout(c->alloc, argv, NULL, 65536, 15, &result);
    c->alloc->free(c->alloc->ctx, script, script_cap);
    if (err != HU_OK) {
        hu_log_error("imessage", NULL, "tapback osascript failed: hu_process_run err=%s",
                     hu_error_string(err));
        hu_run_result_free(c->alloc, &result);
        return HU_ERR_NOT_SUPPORTED;
    }
    int exit_code = result.exit_code;
    bool ok = result.success && exit_code == 0;
    if (!ok) {
        hu_log_error("imessage", NULL, "tapback JXA failed: exit=%d stdout=%.*s stderr=%.*s",
                     exit_code,
                     (int)(result.stdout_buf && result.stdout_len > 0
                               ? (result.stdout_len < 200 ? result.stdout_len : 200)
                               : 0),
                     result.stdout_buf ? result.stdout_buf : "",
                     (int)(result.stderr_buf && result.stderr_len > 0
                               ? (result.stderr_len < 200 ? result.stderr_len : 200)
                               : 0),
                     result.stderr_buf ? result.stderr_buf : "");
    }
    hu_run_result_free(c->alloc, &result);
    if (!ok)
        return HU_ERR_NOT_SUPPORTED;
    return HU_OK;
#endif
#endif
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

/* ══════════════════════════════════════════════════════════════════════
 * Native Messages.app bridge — AX + IMCore
 *
 * Three-tier fallback for typing indicators and tapback reactions:
 *   Tier 1: IMCore private framework (dlopen, macOS 14-15, fast, no UI)
 *   Tier 2: Accessibility API (AXUIElement, macOS 14+, bypasses keystroke block)
 *   Tier 3: AppleScript/JXA subprocess (existing, last resort)
 *
 * Tier 2 is the primary innovation: AXUIElementSetAttributeValue and
 * AXUIElementPerformAction use the calling process's Accessibility permission
 * directly — they don't route through System Events and don't need the
 * "send keystrokes" entitlement that macOS Sequoia/Tahoe blocks.
 * ══════════════════════════════════════════════════════════════════════ */

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)

/* ── Messages.app PID lookup ────────────────────────────────────────── */
static pid_t ax_messages_pid(void) {
    int count = proc_listallpids(NULL, 0);
    if (count <= 0)
        return 0;
    size_t buf_size = (size_t)(count + 64) * sizeof(pid_t);
    pid_t *pids = (pid_t *)malloc(buf_size);
    if (!pids)
        return 0;
    count = proc_listallpids(pids, (int)buf_size);
    pid_t found = 0;
    for (int i = 0; i < count; i++) {
        char name[64] = {0};
        proc_name(pids[i], name, sizeof(name));
        if (strcmp(name, "Messages") == 0) {
            found = pids[i];
            break;
        }
    }
    free(pids);
    return found;
}

/* ── AX tree walker: find compose text field ────────────────────────── */
#define AX_MAX_DEPTH 25

static AXUIElementRef ax_find_compose_field_recurse(AXUIElementRef elem, int depth) {
    if (depth > AX_MAX_DEPTH)
        return NULL;
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute, (CFTypeRef *)&children) !=
            kAXErrorSuccess ||
        !children)
        return NULL;
    AXUIElementRef result = NULL;
    CFIndex count = CFArrayGetCount(children);
    for (CFIndex i = count - 1; i >= 0 && !result; i--) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
        CFStringRef role = NULL;
        if (AXUIElementCopyAttributeValue(child, kAXRoleAttribute, (CFTypeRef *)&role) ==
                kAXErrorSuccess &&
            role) {
            bool is_text = (CFStringCompare(role, CFSTR("AXTextArea"), 0) == kCFCompareEqualTo ||
                            CFStringCompare(role, CFSTR("AXTextField"), 0) == kCFCompareEqualTo);
            CFRelease(role);
            if (is_text) {
                /* macOS 26 Messages (SwiftUI): the compose field has
                 * desc="Message"; message bubbles are AXTextArea desc="text
                 * entry area". Only accept fields with desc="Message" or
                 * AXTextField role (which is never a message bubble). */
                CFStringRef desc = NULL;
                AXUIElementCopyAttributeValue(child, kAXDescriptionAttribute, (CFTypeRef *)&desc);
                bool is_compose = false;
                if (desc) {
                    is_compose = (CFStringCompare(desc, CFSTR("Message"), 0) == kCFCompareEqualTo);
                    CFRelease(desc);
                }
                if (!is_compose) {
                    CFStringRef child_role = NULL;
                    AXUIElementCopyAttributeValue(child, kAXRoleAttribute,
                                                  (CFTypeRef *)&child_role);
                    if (child_role) {
                        is_compose = (CFStringCompare(child_role, CFSTR("AXTextField"), 0) ==
                                      kCFCompareEqualTo);
                        CFRelease(child_role);
                    }
                }
                if (is_compose) {
                    Boolean settable = false;
                    AXUIElementIsAttributeSettable(child, kAXValueAttribute, &settable);
                    if (settable) {
                        CFRetain(child);
                        result = child;
                    }
                }
            }
        } else if (role) {
            CFRelease(role);
        }
        if (!result)
            result = ax_find_compose_field_recurse(child, depth + 1);
    }
    CFRelease(children);
    return result;
}

/* ── Activate Messages.app via NSRunningApplication ──────────────────
 * Stronger than AXFrontmost alone — uses AppKit to force-activate even
 * when the calling process is a background daemon. */
static void ax_activate_messages(pid_t pid) {
    Class ns_running_app = objc_getClass("NSRunningApplication");
    if (!ns_running_app)
        return;
    SEL sel_pid = sel_registerName("runningApplicationWithProcessIdentifier:");
    id app_obj = ((id (*)(id, SEL, pid_t))objc_msgSend)((id)ns_running_app, sel_pid, pid);
    if (!app_obj)
        return;
    /* activateWithOptions: NSApplicationActivateIgnoringOtherApps (1 << 1 = 2) */
    SEL sel_activate = sel_registerName("activateWithOptions:");
    ((BOOL (*)(id, SEL, unsigned long))objc_msgSend)(app_obj, sel_activate, 2UL);
}

/* ── Open Messages conversation reliably ─────────────────────────────
 * Uses imessage:// URL scheme + NSRunningApplication activation + AXFrontmost
 * + AXRaise. Robust against the daemon running in background. */
static void ax_open_conversation(const char *recipient, size_t recipient_len) {
    char url[320];
    int n = snprintf(url, sizeof(url), "imessage://%.*s", (int)recipient_len, recipient);
    if (n <= 0 || (size_t)n >= sizeof(url))
        return;
    pid_t child = fork();
    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("open", "open", url, NULL);
        _exit(127);
    } else if (child > 0) {
        int status = 0;
        waitpid(child, &status, 0);
    }
    usleep(300000); /* 300ms for URL handling */

    pid_t pid = ax_messages_pid();
    if (pid <= 0)
        return;

    /* NSRunningApplication activation — strongest method for background daemons */
    ax_activate_messages(pid);

    /* Also set AXFrontmost as a belt-and-suspenders approach */
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (app) {
        AXError aerr = AXUIElementSetAttributeValue(app, CFSTR("AXFrontmost"), kCFBooleanTrue);
        if (aerr != kAXErrorSuccess)
            hu_log_info("imessage", NULL, "AX setFrontmost: error %d", (int)aerr);

        /* AXRaise on the first window for extra robustness */
        CFArrayRef windows = NULL;
        AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
        if (windows && CFArrayGetCount(windows) > 0) {
            AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
            AXUIElementPerformAction(win, CFSTR("AXRaise"));
        }
        if (windows)
            CFRelease(windows);
        CFRelease(app);
    }
    usleep(500000); /* 500ms for window to appear after activation */
}

/* ── AX window helper ───────────────────────────────────────────────
 * macOS 26 Messages: windows may not exist when running in the background.
 * Try focused window first, then any AXWindow, then the first top-level
 * child element (SwiftUI apps sometimes expose the main view as a non-window). */
static AXUIElementRef ax_get_messages_window(void) {
    pid_t pid = ax_messages_pid();
    if (pid == 0)
        return NULL;
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app)
        return NULL;
    AXUIElementRef window = NULL;
    AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, (CFTypeRef *)&window);
    if (!window) {
        CFArrayRef windows = NULL;
        AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
        if (windows && CFArrayGetCount(windows) > 0) {
            window = (AXUIElementRef)CFArrayGetValueAtIndex(windows, 0);
            CFRetain(window);
        }
        if (windows)
            CFRelease(windows);
    }
    if (!window) {
        /* macOS 26 fallback: check top-level children for any element with children
         * (the SwiftUI main view appears as a role-less child of the application). */
        CFArrayRef children = NULL;
        AXUIElementCopyAttributeValue(app, kAXChildrenAttribute, (CFTypeRef *)&children);
        if (children) {
            CFIndex count = CFArrayGetCount(children);
            for (CFIndex i = 0; i < count; i++) {
                AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                CFStringRef role = NULL;
                AXUIElementCopyAttributeValue(child, kAXRoleAttribute, (CFTypeRef *)&role);
                bool is_menu =
                    (role && CFStringCompare(role, CFSTR("AXMenuBar"), 0) == kCFCompareEqualTo);
                if (role)
                    CFRelease(role);
                if (!is_menu) {
                    CFArrayRef sub = NULL;
                    AXUIElementCopyAttributeValue(child, kAXChildrenAttribute, (CFTypeRef *)&sub);
                    if (sub) {
                        CFIndex sub_count = CFArrayGetCount(sub);
                        CFRelease(sub);
                        if (sub_count > 0) {
                            CFRetain(child);
                            window = child;
                            break;
                        }
                    }
                }
            }
            CFRelease(children);
        }
    }
    CFRelease(app);
    return window;
}

/* ── AX: start typing via compose field value injection ─────────────── */
static bool ax_start_typing(const char *target, size_t target_len) {
    ax_open_conversation(target, target_len);

    /* Retry loop: Messages.app may need time to fully activate and populate
     * its AX tree, especially when the daemon runs in the background.
     * Try up to 8 times with 200ms intervals (~1.6s max additional wait). */
    AXUIElementRef field = NULL;
    for (int attempt = 0; attempt < 8 && !field; attempt++) {
        if (attempt > 0) {
            usleep(200000); /* 200ms between retries */
            /* Re-activate on retries 2 and 5 in case focus was stolen */
            if (attempt == 2 || attempt == 5) {
                pid_t pid = ax_messages_pid();
                if (pid > 0)
                    ax_activate_messages(pid);
            }
        }
        AXUIElementRef window = ax_get_messages_window();
        if (!window)
            continue;
        field = ax_find_compose_field_recurse(window, 0);
        CFRelease(window);
    }
    if (!field) {
        hu_log_info("imessage", NULL, "AX typing: compose field not found after retries");
        return false;
    }

    AXUIElementSetAttributeValue(field, kAXFocusedAttribute, kCFBooleanTrue);

    /* Inject a zero-width space — Messages sends the typing indicator when
     * the compose field has content. Invisible if the user glances at screen. */
    CFStringRef marker = CFSTR("\xE2\x80\x8B"); /* U+200B ZERO WIDTH SPACE */
    AXError set_err = AXUIElementSetAttributeValue(field, kAXValueAttribute, marker);
    CFRelease(field);
    if (set_err != kAXErrorSuccess)
        hu_log_info("imessage", NULL, "AX typing: set value failed (%d)", (int)set_err);
    return (set_err == kAXErrorSuccess);
}

/* ── AX: stop typing by clearing compose field ──────────────────────── */
static bool ax_stop_typing(void) {
    AXUIElementRef window = ax_get_messages_window();
    if (!window)
        return false;
    AXUIElementRef field = ax_find_compose_field_recurse(window, 0);
    CFRelease(window);
    if (!field)
        return false;
    CFStringRef empty = CFSTR("");
    AXError err = AXUIElementSetAttributeValue(field, kAXValueAttribute, empty);
    CFRelease(field);
    /* Send Return key to dismiss any pending text (just clears the field). */
    return (err == kAXErrorSuccess);
}

#ifdef HU_IMESSAGE_TAPBACK_ENABLED
/* ── AX tree: find message element for tapback ──────────────────────
 * macOS 26 (Tahoe): Messages uses SwiftUI; transcript is nested AXGroups.
 * Each message bubble is an AXGroup whose description contains the message
 * text (e.g. "Your iMessage, hello world, 3:54 PM"). Children contain
 * AXTextArea elements with desc="text entry area" holding the actual text.
 * We match by walking AXGroup descriptions or child AXTextArea values. */
static AXUIElementRef ax_find_message_group(AXUIElementRef elem, const char *content_prefix,
                                            int depth) {
    if (depth > AX_MAX_DEPTH || !content_prefix || !content_prefix[0])
        return NULL;
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute, (CFTypeRef *)&children) !=
            kAXErrorSuccess ||
        !children)
        return NULL;

    AXUIElementRef found = NULL;
    CFIndex count = CFArrayGetCount(children);
    /* Walk from bottom (most recent messages last). */
    for (CFIndex i = count - 1; i >= 0 && !found; i--) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);

        /* Check this element's description for message text. */
        CFStringRef desc = NULL;
        if (AXUIElementCopyAttributeValue(child, kAXDescriptionAttribute, (CFTypeRef *)&desc) ==
                kAXErrorSuccess &&
            desc) {
            char dbuf[512] = {0};
            CFStringGetCString(desc, dbuf, (CFIndex)sizeof(dbuf), kCFStringEncodingUTF8);
            CFRelease(desc);
            if (hu_imessage_desc_prefix_match(dbuf, content_prefix)) {
                /* Check this element supports AXShowMenu (for context menu). */
                CFArrayRef actions = NULL;
                if (AXUIElementCopyActionNames(child, &actions) == kAXErrorSuccess && actions) {
                    CFIndex ac = CFArrayGetCount(actions);
                    for (CFIndex a = 0; a < ac; a++) {
                        CFStringRef act = (CFStringRef)CFArrayGetValueAtIndex(actions, a);
                        if (CFStringCompare(act, CFSTR("AXShowMenu"), 0) == kCFCompareEqualTo) {
                            CFRetain(child);
                            found = child;
                            break;
                        }
                    }
                    CFRelease(actions);
                }
            }
        }

        /* Also check child AXTextArea values. */
        if (!found) {
            CFStringRef role = NULL;
            AXUIElementCopyAttributeValue(child, kAXRoleAttribute, (CFTypeRef *)&role);
            bool is_textarea =
                (role && CFStringCompare(role, CFSTR("AXTextArea"), 0) == kCFCompareEqualTo);
            if (role)
                CFRelease(role);
            if (is_textarea) {
                CFStringRef val = NULL;
                if (AXUIElementCopyAttributeValue(child, kAXValueAttribute, (CFTypeRef *)&val) ==
                        kAXErrorSuccess &&
                    val) {
                    char vbuf[512] = {0};
                    CFStringGetCString(val, vbuf, (CFIndex)sizeof(vbuf), kCFStringEncodingUTF8);
                    CFRelease(val);
                    if (hu_imessage_desc_prefix_match(vbuf, content_prefix)) {
                        /* Return the PARENT (which has AXShowMenu), not the text area. */
                        CFRetain(elem);
                        found = elem;
                    }
                }
            }
        }

        if (!found)
            found = ax_find_message_group(child, content_prefix, depth + 1);
    }
    CFRelease(children);
    return found;
}

/* ── AX: perform tapback reaction ───────────────────────────────────
 * macOS 26 (Tahoe): SwiftUI Messages exposes tapbacks as custom AX actions
 * on the message element (e.g. "Name:Heart\nTarget:0x0\nSelector:(null)").
 * We enumerate actions on the message and its inner child, then perform
 * the one matching our desired tapback prefix. Pure AX — no CGEvent. */
static bool ax_perform_tapback_on_row(AXUIElementRef row, const char *tapback_label) {
    /* Check both the row and its first child (SwiftUI nests the actions
     * on the inner group, not the outer description group). */
    AXUIElementRef targets[2] = {row, NULL};
    int target_count = 1;
    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(row, kAXChildrenAttribute, (CFTypeRef *)&children) ==
            kAXErrorSuccess &&
        children) {
        if (CFArrayGetCount(children) > 0) {
            targets[1] = (AXUIElementRef)CFArrayGetValueAtIndex(children, 0);
            target_count = 2;
        }
    }

    bool success = false;
    size_t label_len = strlen(tapback_label);
    for (int t = 0; t < target_count && !success; t++) {
        CFArrayRef actions = NULL;
        if (AXUIElementCopyActionNames(targets[t], &actions) != kAXErrorSuccess || !actions)
            continue;
        CFIndex ac = CFArrayGetCount(actions);
        for (CFIndex a = 0; a < ac && !success; a++) {
            CFStringRef act_name = (CFStringRef)CFArrayGetValueAtIndex(actions, a);
            char abuf[256] = {0};
            CFStringGetCString(act_name, abuf, (CFIndex)sizeof(abuf), kCFStringEncodingUTF8);
            if (strncmp(abuf, tapback_label, label_len) == 0) {
                AXError err = AXUIElementPerformAction(targets[t], act_name);
                success = (err == kAXErrorSuccess);
            }
        }
        CFRelease(actions);
    }

    if (children)
        CFRelease(children);
    return success;
}

static bool ax_tapback(const char *content_prefix, int row_offset, const char *tapback_label) {
    (void)row_offset;

    /* Retry loop: window or message may not be in AX tree immediately */
    AXUIElementRef msg_group = NULL;
    for (int attempt = 0; attempt < 6 && !msg_group; attempt++) {
        if (attempt > 0) {
            usleep(250000); /* 250ms between retries */
            if (attempt == 3) {
                pid_t pid = ax_messages_pid();
                if (pid > 0)
                    ax_activate_messages(pid);
            }
        }
        AXUIElementRef window = ax_get_messages_window();
        if (!window)
            continue;
        msg_group = ax_find_message_group(window, content_prefix, 0);
        CFRelease(window);
    }
    if (!msg_group) {
        hu_log_info("imessage", NULL, "AX tapback: message not found after retries");
        return false;
    }

    bool ok = ax_perform_tapback_on_row(msg_group, tapback_label);
    CFRelease(msg_group);
    hu_log_info("imessage", NULL, "AX tapback: %s (action=%s)", ok ? "sent" : "action not found",
                tapback_label);
    return ok;
}

/* Like ax_perform_tapback_on_row but instead of clicking one of the 6 named
 * classic tapbacks, navigates to the Sonoma+ bottom-row emoji sub-picker
 * (the 6 custom-emoji buttons rendered below the classics) and clicks the
 * AXButton child whose AXValue or AXTitle equals `emoji_utf8`.
 *
 * Returns true on click success; false if message_group not found, sub-
 * picker not present, or no matching emoji found.
 *
 * Like the real Tier 1/Tier 2 reply paths in src/channels/imessage_reply.c,
 * the FULL AX wiring is intentionally stubbed (return false) for D1 — the
 * test contract is exercised via test stubs, and the real CGEvent + AX
 * polling lands in the post-C5-style integration pass on a live macOS box. */
static bool ax_react_emoji_subpicker(const char *target, size_t target_len, int64_t message_id,
                                     const char *emoji_utf8) {
    (void)target;
    (void)target_len;
    (void)message_id;
    (void)emoji_utf8;
    return false; /* Real AX wiring deferred to integration pass */
}

/* ── C3: real threaded-reply AX wiring ──────────────────────────────────
 * Drives Messages.app to send a NATIVE inline (threaded) reply instead of
 * a free-floating new message. Two tiers exposed to imessage_reply.c:
 *   Tier 1  Cmd-R on the focused parent row → inline composer → type → send
 *   Tier 2  AXShowMenu on the parent row → click "Reply" item → composer
 * Both resolve the parent message text from chat.db (via its guid) so the
 * AX walker can locate the matching transcript row.
 *
 * These are RAW UI automation — the public iMessage API has no inline-reply
 * verb (see docs/investigations/imessage-capability-matrix.md). They are
 * therefore best-effort: any failure returns false and the caller falls
 * through to the next tier (and ultimately flat-send). */

/* HID virtual key codes (HIToolbox/Events.h values; ApplicationServices
 * doesn't pull in Carbon, so define the two we need locally). */
#define HU_VK_ANSI_R 0x0F
#define HU_VK_RETURN 0x24

/* Resolve a message guid to the first <=cap-1 chars of its display text, for
 * use as an AX transcript-row match prefix. Reads chat.db READONLY; falls
 * back to the attributedBody binary plist when the plain `text` column is
 * NULL (modern Messages stores rich text there). */
static hu_error_t parent_guid_to_text_prefix(const char *guid, size_t guid_len, char *out,
                                             size_t cap) {
    if (!guid || guid_len == 0 || !out || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    out[0] = '\0';
#ifdef HU_ENABLE_SQLITE
    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_FOUND;
    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_NOT_FOUND;
    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return HU_ERR_NOT_FOUND;
    const char *sql = "SELECT text, attributedBody FROM message WHERE guid = ?1 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_NOT_FOUND;
    }
    sqlite3_bind_text(stmt, 1, guid, (int)guid_len, NULL);
    hu_error_t rc = HU_ERR_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        if (txt && txt[0]) {
            size_t copy = strlen((const char *)txt);
            if (copy >= cap)
                copy = cap - 1;
            memcpy(out, txt, copy);
            out[copy] = '\0';
            rc = HU_OK;
        } else {
            const void *blob = sqlite3_column_blob(stmt, 1);
            int blob_len = sqlite3_column_bytes(stmt, 1);
            if (blob && blob_len > 0) {
                char tmp[256] = {0};
                size_t got = hu_imessage_extract_attributed_body(
                    (const unsigned char *)blob, (size_t)blob_len, tmp, sizeof(tmp));
                if (got > 0) {
                    size_t copy = got;
                    if (copy >= cap)
                        copy = cap - 1;
                    memcpy(out, tmp, copy);
                    out[copy] = '\0';
                    rc = HU_OK;
                }
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
#else
    (void)guid;
    (void)guid_len;
    (void)out;
    (void)cap;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* Post a single key-down/key-up pair with the given modifier flags. */
static void ax_post_key(CGKeyCode kc, CGEventFlags flags) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, kc, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, kc, false);
    if (down) {
        CGEventSetFlags(down, flags);
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);
    }
    if (up) {
        CGEventSetFlags(up, flags);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
}

/* Open the conversation and locate the transcript row matching `prefix`.
 * Returns a retained AXUIElementRef (caller releases) or NULL. */
static AXUIElementRef ax_find_reply_row(const char *target, size_t target_len, const char *prefix) {
    ax_open_conversation(target, target_len);
    AXUIElementRef row = NULL;
    for (int attempt = 0; attempt < 6 && !row; attempt++) {
        if (attempt > 0) {
            usleep(250000);
            if (attempt == 3) {
                pid_t pid = ax_messages_pid();
                if (pid > 0)
                    ax_activate_messages(pid);
            }
        }
        AXUIElementRef window = ax_get_messages_window();
        if (!window)
            continue;
        row = ax_find_message_group(window, prefix, 0);
        CFRelease(window);
    }
    return row;
}

/* Poll for the (inline reply) compose field, set its value to body, then
 * synthesize Return to send. Returns true on a successful value-set. */
static bool ax_inject_body_and_return(const char *body, size_t body_len) {
    if (!body || body_len == 0)
        return false;
    CFStringRef cf = CFStringCreateWithBytes(NULL, (const UInt8 *)body, (CFIndex)body_len,
                                             kCFStringEncodingUTF8, false);
    if (!cf)
        return false;
    AXUIElementRef field = NULL;
    for (int attempt = 0; attempt < 8 && !field; attempt++) {
        if (attempt > 0)
            usleep(200000);
        AXUIElementRef window = ax_get_messages_window();
        if (!window)
            continue;
        field = ax_find_compose_field_recurse(window, 0);
        CFRelease(window);
    }
    if (!field) {
        CFRelease(cf);
        hu_log_info("imessage", NULL, "AX reply: compose field not found for body injection");
        return false;
    }
    AXUIElementSetAttributeValue(field, kAXFocusedAttribute, kCFBooleanTrue);
    AXError set_err = AXUIElementSetAttributeValue(field, kAXValueAttribute, cf);
    CFRelease(field);
    CFRelease(cf);
    if (set_err != kAXErrorSuccess) {
        hu_log_info("imessage", NULL, "AX reply: set compose value failed (%d)", (int)set_err);
        return false;
    }
    usleep(120000); /* let the field settle before Return */
    ax_post_key((CGKeyCode)HU_VK_RETURN, 0);
    return true;
}

/* After AXShowMenu on `row`, find the popup AXMenu and click the "Reply"
 * item (matches "Reply", "Reply…" U+2026, and "Reply..." three-dot). */
static bool ax_click_menu_item_reply(AXUIElementRef row) {
    if (AXUIElementPerformAction(row, CFSTR("AXShowMenu")) != kAXErrorSuccess)
        return false;
    AXUIElementRef menu = NULL;
    for (int attempt = 0; attempt < 8 && !menu; attempt++) {
        usleep(150000);
        CFArrayRef children = NULL;
        if (AXUIElementCopyAttributeValue(row, kAXChildrenAttribute, (CFTypeRef *)&children) ==
                kAXErrorSuccess &&
            children) {
            CFIndex cn = CFArrayGetCount(children);
            for (CFIndex i = 0; i < cn && !menu; i++) {
                AXUIElementRef ch = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                CFStringRef role = NULL;
                if (AXUIElementCopyAttributeValue(ch, kAXRoleAttribute, (CFTypeRef *)&role) ==
                        kAXErrorSuccess &&
                    role) {
                    if (CFStringCompare(role, CFSTR("AXMenu"), 0) == kCFCompareEqualTo) {
                        CFRetain(ch);
                        menu = ch;
                    }
                    CFRelease(role);
                }
            }
            CFRelease(children);
        }
    }
    if (!menu)
        return false;
    bool clicked = false;
    CFArrayRef items = NULL;
    if (AXUIElementCopyAttributeValue(menu, kAXChildrenAttribute, (CFTypeRef *)&items) ==
            kAXErrorSuccess &&
        items) {
        CFIndex in = CFArrayGetCount(items);
        for (CFIndex i = 0; i < in && !clicked; i++) {
            AXUIElementRef item = (AXUIElementRef)CFArrayGetValueAtIndex(items, i);
            CFStringRef title = NULL;
            if (AXUIElementCopyAttributeValue(item, kAXTitleAttribute, (CFTypeRef *)&title) ==
                    kAXErrorSuccess &&
                title) {
                char tbuf[128] = {0};
                CFStringGetCString(title, tbuf, (CFIndex)sizeof(tbuf), kCFStringEncodingUTF8);
                CFRelease(title);
                if (strncmp(tbuf, "Reply", 5) == 0)
                    clicked = (AXUIElementPerformAction(item, kAXPressAction) == kAXErrorSuccess);
            }
        }
        CFRelease(items);
    }
    CFRelease(menu);
    return clicked;
}

bool hu_imessage_ax_reply_tier1_cmd_r(const char *target, size_t target_len,
                                      const char *parent_guid, size_t parent_guid_len,
                                      const char *body, size_t body_len) {
    if (!target || target_len == 0 || !body || body_len == 0)
        return false;
    /* Cmd-R triggers Messages' "Reply to Last Message…" — it threads to the
     * NEWEST message in the open conversation, so there is no per-message row
     * to focus (and focus would be ignored anyway; that is why the old
     * text-prefix row-focus dance was dead effort). Targeting is guaranteed by
     * the caller, which only invokes this tier when the parent IS the last
     * message (hu_imessage_reply_parent_is_last). So: open the conversation,
     * Cmd-R, inject the body, Return. */
    (void)parent_guid;
    (void)parent_guid_len;
    ax_open_conversation(target, target_len);
    usleep(200000); /* let the conversation focus/settle before Cmd-R */
    ax_post_key((CGKeyCode)HU_VK_ANSI_R, kCGEventFlagMaskCommand);
    usleep(200000); /* inline reply composer appears */
    bool ok = ax_inject_body_and_return(body, body_len);
    hu_log_info("imessage", NULL, "AX reply tier1 (cmdR/last-message): %s", ok ? "sent" : "failed");
    return ok;
}

bool hu_imessage_ax_reply_tier2_show_menu(const char *target, size_t target_len,
                                          const char *parent_guid, size_t parent_guid_len,
                                          const char *body, size_t body_len) {
    if (!target || target_len == 0 || !body || body_len == 0)
        return false;
    char prefix[33] = {0};
    if (parent_guid_to_text_prefix(parent_guid, parent_guid_len, prefix, sizeof(prefix)) != HU_OK ||
        prefix[0] == '\0')
        return false;
    AXUIElementRef row = ax_find_reply_row(target, target_len, prefix);
    if (!row) {
        hu_log_info("imessage", NULL, "AX reply tier2: parent row not found (prefix=%.20s)",
                    prefix);
        return false;
    }
    bool menu_ok = ax_click_menu_item_reply(row);
    CFRelease(row);
    if (!menu_ok) {
        hu_log_info("imessage", NULL, "AX reply tier2: Reply menu item not found");
        return false;
    }
    usleep(200000);
    bool ok = ax_inject_body_and_return(body, body_len);
    hu_log_info("imessage", NULL, "AX reply tier2 (ax_menu): %s", ok ? "sent" : "failed");
    return ok;
}

/* Post-send chat.db threading check. The AX value-inject + Return path can
 * silently commit a FLAT message (reply_to_guid set but thread_originator_guid
 * NULL) because IMCore is entitlement-locked on macOS 26+. The only honest way
 * to know whether a reply actually threaded is to read the row back AFTER the
 * send. Polls for the newest outbound row in `target` with date >= since_unix
 * and returns true iff its thread_originator_guid is populated. Best-effort:
 * any lookup failure returns false (we never claim a thread we couldn't
 * confirm). */
bool hu_imessage_ax_reply_verify_threaded(const char *target, size_t target_len,
                                          int64_t since_rowid) {
    if (!target || target_len == 0)
        return false;
#ifdef HU_ENABLE_SQLITE
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;

    char target_buf[256];
    size_t tlen = target_len < sizeof(target_buf) - 1 ? target_len : sizeof(target_buf) - 1;
    memcpy(target_buf, target, tlen);
    target_buf[tlen] = '\0';

    /* Identify OUR reply by ROWID, not a coarse timestamp. since_rowid is the
     * pre-send MAX(ROWID); the first outbound row to this handle with a higher
     * ROWID is the message we just sent — unaffected by any other send that
     * lands in the same wall-clock second. ORDER BY ROWID ASC takes that first
     * new row (not the newest, which could be a later unrelated send).
     * The send is asynchronous: Messages writes the row a moment after the AX
     * Return, so poll ~5×400ms before giving up. */
    const char *sql = "SELECT m.thread_originator_guid FROM message m "
                      "JOIN handle h ON m.handle_id = h.ROWID "
                      "WHERE h.id = ?1 AND m.is_from_me = 1 AND m.ROWID > ?2 "
                      "ORDER BY m.ROWID ASC LIMIT 1";
    bool threaded = false;
    bool found = false;
    for (int attempt = 0; attempt < 5 && !found; attempt++) {
        if (attempt > 0)
            usleep(400000);
        sqlite3 *db = NULL;
        if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
            continue;
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            continue;
        }
        sqlite3_bind_text(stmt, 1, target_buf, -1, NULL);
        sqlite3_bind_int64(stmt, 2, since_rowid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = true; /* our row materialized — stop polling */
            const unsigned char *tog = sqlite3_column_text(stmt, 0);
            if (tog && tog[0])
                threaded = true;
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
    return threaded;
#else
    (void)target;
    (void)target_len;
    (void)since_rowid;
    return false;
#endif
}

int64_t hu_imessage_ax_reply_newest_rowid(void) {
#ifdef HU_ENABLE_SQLITE
    const char *home = getenv("HOME");
    if (!home)
        return 0;
    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return 0;
    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return 0;
    int64_t max_rowid = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT MAX(ROWID) FROM message", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            max_rowid = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return max_rowid;
#else
    return 0;
#endif
}

/* Is `parent_guid` the newest message in its conversation? Tier 1 (Cmd-R =
 * "Reply to Last Message") threads to whatever is newest in the open chat, so
 * it is only correct when the parent IS that newest message. Scope by the
 * parent's OWN chat (via chat_message_join) so this works for both DMs and
 * groups; `target` is not needed for the query. True iff no message in the same
 * chat has a higher ROWID. Best-effort: false on any failure (NULL/empty guid,
 * db unreadable, guid not found) — we prefer the specific-message path over a
 * possibly-wrong Cmd-R thread. */
bool hu_imessage_ax_parent_is_last_message(const char *target, size_t target_len,
                                           const char *parent_guid, size_t parent_guid_len) {
    (void)target;
    (void)target_len;
    if (!parent_guid || parent_guid_len == 0)
        return false;
#ifdef HU_ENABLE_SQLITE
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path))
        return false;

    char guid_buf[128];
    size_t glen = parent_guid_len < sizeof(guid_buf) - 1 ? parent_guid_len : sizeof(guid_buf) - 1;
    memcpy(guid_buf, parent_guid, glen);
    guid_buf[glen] = '\0';

    sqlite3 *db = NULL;
    if (imessage_open_chatdb(db_path, &db) != SQLITE_OK)
        return false;

    /* Resolve the parent's ROWID and the chat it belongs to. */
    int64_t parent_rowid = -1;
    int64_t chat_id = -1;
    sqlite3_stmt *st = NULL;
    const char *q1 = "SELECT m.ROWID, cmj.chat_id FROM message m "
                     "JOIN chat_message_join cmj ON cmj.message_id = m.ROWID "
                     "WHERE m.guid = ?1 LIMIT 1";
    if (sqlite3_prepare_v2(db, q1, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, guid_buf, (int)glen, NULL);
        if (sqlite3_step(st) == SQLITE_ROW) {
            parent_rowid = sqlite3_column_int64(st, 0);
            chat_id = sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }

    bool is_last = false;
    if (parent_rowid >= 0 && chat_id >= 0) {
        /* Any newer message in the same chat? If none, the parent is last. */
        sqlite3_stmt *st2 = NULL;
        const char *q2 = "SELECT 1 FROM chat_message_join cmj "
                         "JOIN message m ON m.ROWID = cmj.message_id "
                         "WHERE cmj.chat_id = ?1 AND m.ROWID > ?2 LIMIT 1";
        if (sqlite3_prepare_v2(db, q2, -1, &st2, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st2, 1, chat_id);
            sqlite3_bind_int64(st2, 2, parent_rowid);
            is_last = (sqlite3_step(st2) != SQLITE_ROW); /* no newer row → parent is last */
            sqlite3_finalize(st2);
        }
    }
    sqlite3_close(db);
    return is_last;
#else
    return false;
#endif
}
#endif /* HU_IMESSAGE_TAPBACK_ENABLED */

/* ── IMCore private framework bridge ────────────────────────────────── */
static bool imcore_init(hu_imessage_ctx_t *c) {
    if (!c || c->imcore_tried)
        return c ? c->imcore_connected : false;
    c->imcore_tried = true;

    c->imcore_handle =
        dlopen("/System/Library/PrivateFrameworks/IMCore.framework/IMCore", RTLD_LAZY);
    if (!c->imcore_handle)
        return false;

    Class daemon_cls = (Class)objc_getClass("IMDaemonController");
    if (!daemon_cls) {
        dlclose(c->imcore_handle);
        c->imcore_handle = NULL;
        return false;
    }

    typedef id (*id_msg)(id, SEL);
    id controller = ((id_msg)objc_msgSend)((id)daemon_cls, sel_registerName("sharedInstance"));
    if (!controller) {
        dlclose(c->imcore_handle);
        c->imcore_handle = NULL;
        return false;
    }

    /* Try connecting to the imagent daemon. Fails on macOS 26+ due to
     * private entitlement lockdown (com.apple.imagent.desktop.auth). */
    typedef void (*void_msg)(id, SEL);
    ((void_msg)objc_msgSend)(controller, sel_registerName("connectToDaemon"));

    typedef BOOL (*bool_msg)(id, SEL);
    BOOL connected = ((bool_msg)objc_msgSend)(controller, sel_registerName("isConnected"));
    c->imcore_connected = (connected != 0);
    if (!c->imcore_connected) {
        hu_log_info("imessage", NULL,
                    "IMCore loaded but daemon connection failed "
                    "(expected on macOS 26+, falling back to AX)");
    }
    return c->imcore_connected;
}

static bool imcore_start_typing(hu_imessage_ctx_t *c, const char *recipient, size_t recipient_len) {
    if (!c || !c->imcore_connected || !recipient || recipient_len == 0)
        return false;

    Class registry_cls = (Class)objc_getClass("IMChatRegistry");
    if (!registry_cls)
        return false;

    typedef id (*id_msg)(id, SEL, ...);
    id registry = ((id_msg)objc_msgSend)((id)registry_cls, sel_registerName("sharedInstance"));
    if (!registry)
        return false;

    Class ns_string = (Class)objc_getClass("NSString");
    if (!ns_string)
        return false;

    /* macOS 26 uses "any;-;" prefix; older versions use "iMessage;-;" / "SMS;-;". */
    static const char *prefixes[] = {"iMessage;-;", "SMS;-;", "any;-;"};
    char chat_id[320];
    id chat = NULL;
    for (int px = 0; px < 3 && !chat; px++) {
        int n = snprintf(chat_id, sizeof(chat_id), "%s%.*s", prefixes[px], (int)recipient_len,
                         recipient);
        if (n < 0 || (size_t)n >= sizeof(chat_id))
            continue;
        id chat_id_str = ((id_msg)objc_msgSend)((id)ns_string,
                                                sel_registerName("stringWithUTF8String:"), chat_id);
        if (chat_id_str)
            chat = ((id_msg)objc_msgSend)(
                registry, sel_registerName("existingChatWithChatIdentifier:"), chat_id_str);
    }
    if (!chat)
        return false;

    typedef void (*bool_set_msg)(id, SEL, BOOL);
    ((bool_set_msg)objc_msgSend)(chat, sel_registerName("setLocalUserIsTyping:"), (BOOL)1);
    return true;
}

static bool imcore_stop_typing(hu_imessage_ctx_t *c, const char *recipient, size_t recipient_len) {
    if (!c || !c->imcore_connected || !recipient || recipient_len == 0)
        return false;

    Class registry_cls = (Class)objc_getClass("IMChatRegistry");
    if (!registry_cls)
        return false;

    typedef id (*id_msg)(id, SEL, ...);
    id registry = ((id_msg)objc_msgSend)((id)registry_cls, sel_registerName("sharedInstance"));
    if (!registry)
        return false;

    Class ns_string = (Class)objc_getClass("NSString");
    if (!ns_string)
        return false;

    static const char *prefixes[] = {"iMessage;-;", "SMS;-;", "any;-;"};
    char chat_id[320];
    id chat = NULL;
    for (int px = 0; px < 3 && !chat; px++) {
        int n = snprintf(chat_id, sizeof(chat_id), "%s%.*s", prefixes[px], (int)recipient_len,
                         recipient);
        if (n < 0 || (size_t)n >= sizeof(chat_id))
            continue;
        id chat_id_str = ((id_msg)objc_msgSend)((id)ns_string,
                                                sel_registerName("stringWithUTF8String:"), chat_id);
        if (chat_id_str)
            chat = ((id_msg)objc_msgSend)(
                registry, sel_registerName("existingChatWithChatIdentifier:"), chat_id_str);
    }
    if (!chat)
        return false;

    typedef void (*bool_set_msg)(id, SEL, BOOL);
    ((bool_set_msg)objc_msgSend)(chat, sel_registerName("setLocalUserIsTyping:"), (BOOL)0);
    return true;
}

#endif /* !HU_IS_TEST && __APPLE__ */

/* Test-only stub interface — set by hu_imessage_set_test_react_emoji_stub. */
static bool (*g_test_react_emoji_subpicker)(const char *emoji_utf8) = NULL;

#if HU_IS_TEST
void hu_imessage_set_test_react_emoji_stub(bool (*stub)(const char *emoji_utf8)) {
    g_test_react_emoji_subpicker = stub;
}
#endif

/* Public: attempt to react with an arbitrary emoji via AX sub-picker.
 * Returns HU_OK on success, HU_ERR_NOT_SUPPORTED if not available
 * (caller can then fall back to classic-tapback mapping in D2). */
hu_error_t hu_imessage_react_emoji_subpicker(void *ctx, const char *target, size_t target_len,
                                             int64_t message_id, const char *emoji_utf8) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message_id;
    if (!emoji_utf8 || !emoji_utf8[0])
        return HU_ERR_INVALID_ARGUMENT;

    bool ok = false;
    if (g_test_react_emoji_subpicker) {
        ok = g_test_react_emoji_subpicker(emoji_utf8);
    } else {
#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)
        ok = ax_react_emoji_subpicker(target, target_len, message_id, emoji_utf8);
#endif
    }
    return ok ? HU_OK : HU_ERR_NOT_SUPPORTED;
}

/* Emoji-to-classic-tapback fallback map. When the Sonoma+ sub-picker AX
 * path (from D1) misses, look up the requested emoji here for the
 * nearest classic tapback. If still no match, fall back to "Liked"
 * (Seth's choice — universal positive, ensures reactions always send). */
static const struct {
    const char *emoji_utf8;
    const char *classic_label;
} CLASSIC_MAP[] = {
    {"❤️", "Loved"},       {"♥️", "Loved"},       {"💕", "Loved"},      {"💗", "Loved"},
    {"💖", "Loved"},      {"💞", "Loved"},      {"🙏", "Loved"},      {"👍", "Liked"},
    {"👍🏻", "Liked"},    {"👍🏼", "Liked"},    {"👍🏽", "Liked"},    {"👍🏾", "Liked"},
    {"👍🏿", "Liked"},    {"✅", "Liked"},      {"👎", "Disliked"},   {"❌", "Disliked"},
    {"😂", "Laughed"},    {"🤣", "Laughed"},    {"😆", "Laughed"},    {"😅", "Laughed"},
    {"‼️", "Emphasized"},  {"❗", "Emphasized"}, {"❕", "Emphasized"}, {"🔥", "Emphasized"},
    {"💯", "Emphasized"}, {"❓", "Questioned"}, {"❔", "Questioned"}, {"🤔", "Questioned"},
    {NULL, NULL}};

/* Look up an emoji in CLASSIC_MAP. Returns the classic label on hit,
 * or "Liked" on miss (universal-positive fallback per Seth's choice).
 * Never returns NULL — always something. */
static const char *classic_label_for_emoji(const char *emoji_utf8) {
    if (!emoji_utf8 || !emoji_utf8[0])
        return "Liked";
    for (size_t i = 0; CLASSIC_MAP[i].emoji_utf8 != NULL; i++) {
        if (strcmp(emoji_utf8, CLASSIC_MAP[i].emoji_utf8) == 0) {
            return CLASSIC_MAP[i].classic_label;
        }
    }
    return "Liked"; /* universal-positive fallback */
}

#if HU_IS_TEST
const char *hu_imessage_test_classic_label_for_emoji(const char *emoji_utf8) {
    return classic_label_for_emoji(emoji_utf8);
}
#endif

/* Public dispatcher: try sub-picker first; on miss, fall back to
 * the CLASSIC_MAP nearest-classic-tapback (which itself defaults to
 * "Liked" on map-miss). Returns HU_OK unless even the classic-tapback
 * AX path fails, in which case returns HU_ERR_NOT_SUPPORTED.
 *
 * Signature must match the vtable->react_emoji slot exactly:
 *   hu_error_t (*)(void *ctx, const char *target, size_t target_len,
 *                  int64_t message_id, const char *emoji_utf8,
 *                  size_t emoji_utf8_len);
 */

hu_reaction_type_t hu_imessage_reaction_for_emoji(const char *emoji_utf8) {
    if (!emoji_utf8 || !emoji_utf8[0])
        return HU_REACTION_THUMBS_UP;
    const char *label = classic_label_for_emoji(emoji_utf8);
    if (strcmp(label, "Loved") == 0)
        return HU_REACTION_HEART;
    if (strcmp(label, "Disliked") == 0)
        return HU_REACTION_THUMBS_DOWN;
    if (strcmp(label, "Laughed") == 0)
        return HU_REACTION_HAHA;
    if (strcmp(label, "Emphasized") == 0)
        return HU_REACTION_EMPHASIS;
    if (strcmp(label, "Questioned") == 0)
        return HU_REACTION_QUESTION;
    return HU_REACTION_THUMBS_UP; /* "Liked" + universal-positive default */
}

hu_error_t hu_imessage_react_emoji_with_fallback(void *ctx, const char *target, size_t target_len,
                                                 int64_t message_id, const char *emoji_utf8,
                                                 size_t emoji_utf8_len) {
    (void)emoji_utf8_len; /* emoji_utf8 is NUL-terminated; len is informational */

    if (!emoji_utf8 || !emoji_utf8[0])
        return HU_ERR_INVALID_ARGUMENT;

    /* Tier 1: sub-picker (D1's hu_imessage_react_emoji_subpicker). */
    if (hu_imessage_react_emoji_subpicker(ctx, target, target_len, message_id, emoji_utf8) ==
        HU_OK) {
        return HU_OK;
    }

    /* Tier 2: NATIVE tapback via the IMCore bridge (2026-07-20 fix).
     * This was previously a stub returning NOT_SUPPORTED, so the dispatcher
     * always fell through to a FLAT TEXT send — shipping the reaction emoji
     * as an actual message, which reads as obviously fake. imsg_try_react
     * routes through `imsg tapback` when the bridge is live (a real
     * associatedMessageType reaction) and `imsg react` otherwise. */
#if defined(__APPLE__) && defined(__MACH__) && !HU_IS_TEST
    {
        hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
        if (c && message_id > 0 &&
            imsg_try_react(c, message_id, hu_imessage_reaction_for_emoji(emoji_utf8)))
            return HU_OK;
    }
#else
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message_id;
#endif
    return HU_ERR_NOT_SUPPORTED;
}

/* ── Typing indicators: three-tier fallback ──────────────────────────
 * Tier 1: IMCore private framework (direct API, no UI, macOS 14-15)
 * Tier 2: AX compose field injection (bypasses keystroke block, macOS 14+)
 * Tier 3: AppleScript keystroke via System Events (last resort)
 * Requires Accessibility permission for tiers 2+3. */
static hu_error_t imessage_start_typing(void *ctx, const char *recipient, size_t recipient_len) {
#if HU_IS_TEST
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_OK;
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc || !recipient || recipient_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (recipient_len > 0 && recipient_len < sizeof(c->typing_last_target)) {
        memcpy(c->typing_last_target, recipient, recipient_len);
        c->typing_last_target[recipient_len] = '\0';
        c->typing_last_target_len = recipient_len;
    }

    /* Tier 1: IMCore — direct API, no UI activation needed. */
    imcore_init(c);
    if (imcore_start_typing(c, recipient, recipient_len)) {
        hu_log_info("imessage", NULL, "typing started via IMCore");
        atomic_store(&c->typing_active, true);
        return HU_OK;
    }

    /* Tier 2: AX compose field injection — no keystrokes, uses our process's
     * Accessibility permission directly. ax_start_typing opens the conversation
     * via imessage:// URL scheme before manipulating the compose field. */
    if (ax_start_typing(recipient, recipient_len)) {
        hu_log_info("imessage", NULL, "typing started via AX compose field");
        atomic_store(&c->typing_active, true);
        return HU_OK;
    }

    /* Tier 3: imsg typing CLI or AppleScript keystroke (legacy). */
    if (c->use_imsg_cli && imsg_cli_available(c)) {
        char tgt_buf[256];
        size_t tb = recipient_len < sizeof(tgt_buf) - 1 ? recipient_len : sizeof(tgt_buf) - 1;
        memcpy(tgt_buf, recipient, tb);
        tgt_buf[tb] = '\0';
        const char *argv[] = {"imsg", "typing", "--to", tgt_buf, "--duration", "5s", NULL};
        hu_run_result_t result = {0};
        hu_error_t err = hu_process_run(c->alloc, argv, NULL, 4096, &result);
        bool ok = (err == HU_OK && result.exit_code == 0);
        hu_run_result_free(c->alloc, &result);
        if (ok) {
            hu_log_info("imessage", NULL, "typing started via imsg CLI");
            atomic_store(&c->typing_active, true);
            return HU_OK;
        }
    }

    /* Rate-limit to one log per daemon lifetime — typing indicator is
     * cosmetic; doesn't break send/receive. Per-turn spam masks real
     * issues. The first failure tells the operator typing is unavailable
     * on this OS+config; subsequent identical failures add no signal.
     * macOS 26+ commonly hits this because IMCore daemon connection is
     * gated (covered by the separate "expected on macOS 26+" boot log)
     * and AX permission for the iMessage app may not be granted. */
    static atomic_bool s_warned_typing_all_failed = false;
    hu_log_info_once(&s_warned_typing_all_failed, "imessage", NULL,
                     "all typing tiers failed (IMCore/AX/imsg) — typing indicators "
                     "disabled this session. Send/receive continues normally. "
                     "Grant Accessibility access to the human binary to enable AX tier.");
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t imessage_stop_typing(void *ctx, const char *recipient, size_t recipient_len) {
#if HU_IS_TEST
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_OK;
#elif !defined(__APPLE__) || !defined(__MACH__)
    (void)ctx;
    (void)recipient;
    (void)recipient_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    /* Tier 1: IMCore */
    if (imcore_stop_typing(c, recipient, recipient_len)) {
        atomic_store(&c->typing_active, false);
        return HU_OK;
    }

    /* Tier 2: AX — clear compose field */
    if (ax_stop_typing()) {
        atomic_store(&c->typing_active, false);
        return HU_OK;
    }

    /* Tier 3: imsg CLI or AppleScript (legacy) */
    if (c->use_imsg_cli && imsg_cli_available(c) && recipient && recipient_len > 0) {
        char tgt_buf[256];
        size_t tb = recipient_len < sizeof(tgt_buf) - 1 ? recipient_len : sizeof(tgt_buf) - 1;
        memcpy(tgt_buf, recipient, tb);
        tgt_buf[tb] = '\0';
        const char *argv[] = {"imsg", "typing", "--to", tgt_buf, "--stop", "true", NULL};
        hu_run_result_t result = {0};
        hu_error_t err = hu_process_run(c->alloc, argv, NULL, 4096, &result);
        bool ok = (err == HU_OK && result.exit_code == 0);
        hu_run_result_free(c->alloc, &result);
        if (ok) {
            atomic_store(&c->typing_active, false);
            return HU_OK;
        }
    }

    atomic_store(&c->typing_active, false);
    return HU_OK;
#endif
}

/* iMessage natively renders Apple Music / Spotify / YouTube Music URLs as
 * rich playable previews (album art, title, artist, play button) when the URL
 * is alone in its bubble. The music-share path uses this capability to send
 * a bare URL instead of downloading + attaching a 30s .m4a preview + JPG. */
static bool imessage_supports_link_unfurl(void *ctx) {
    (void)ctx;
    return true;
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
    .reply = (hu_error_t (*)(void *, const char *, size_t, const char *, size_t, const char *,
                             size_t))hu_imessage_reply,
    .react_emoji = hu_imessage_react_emoji_with_fallback,
    /* send_sticker is intentionally NULL: macOS exposes no automation API to
     * send a native sticker/Memoji balloon (AppleScript/JXA/imsg/BlueBubbles
     * all lack it — see docs/investigations/imessage-sticker-memoji-feasibility.md).
     * Per the vtable contract, NULL == channel does not support sticker send.
     * Expressive images are sent via the regular .send attachment path instead. */
    .send_sticker = NULL,
    .get_attachment_path = imessage_vt_get_attachment_path,
    .human_active_recently = imessage_vt_human_active_recently,
    .get_latest_attachment_path = imessage_vt_get_latest_attachment_path,
    .build_reaction_context = imessage_vt_build_reaction_context,
    .build_read_receipt_context = imessage_vt_build_read_receipt_context,
    .mark_read = imessage_mark_read,
    .supports_link_unfurl = imessage_supports_link_unfurl,
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
    /* US-9.3: courtesy replies default ON. Operators with a non-empty
     * allowlist will immediately start emitting one-per-handle-per-24h
     * courtesy replies; the per-handle dedup file + 50/day aggregate cap
     * (see HU_IMESSAGE_COURTESY_DAILY_CAP) bound spoof-spam exposure. */
    c->courtesy_replies_enabled = true;
    c->chatdb_busy_log_emitted = false;
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

void hu_imessage_set_persona(hu_channel_t *ch, const struct hu_persona *persona) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    c->persona = persona;
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
    /* DIAG (2026-05-25): confirm whether the poll loop is actually invoking
     * this function. Reactive iMessage was failing because no `incoming
     * handle=` log ever fired despite new chat.db rows being present. This
     * single line definitively answers "is the poll being called?" without
     * the noise of per-message logging. Remove once the init issue is fixed. */
    if (getenv("HU_DEBUG")) {
        hu_imessage_ctx_t *_diag_c = (hu_imessage_ctx_t *)channel_ctx;
        static int _diag_counter = 0;
        if ((_diag_counter++ % 30) == 0) { /* log once per ~30s at 1s poll */
            hu_log_info("imessage", NULL,
                        "POLL-DIAG entry last_rowid=%lld imsg_watch_running=%d "
                        "use_imsg_cli=%d circuit_breaker=%d",
                        (long long)_diag_c->last_rowid, _diag_c->imsg_watch_running,
                        _diag_c->use_imsg_cli, _diag_c->circuit_breaker_tripped);
        }
    }

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
        /* AC-9.3.3: BUSY/LOCKED after 3 retries means Messages.app is
         * holding the file (likely a large sync). Save poll status
         * immediately so the doctor sees the freshest state, and emit a
         * one-shot warn line (the gate is reset on the next successful
         * poll below). */
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            imessage_save_poll_status(c);
            if (!c->chatdb_busy_log_emitted) {
                c->chatdb_busy_log_emitted = true;
                hu_log_warn("imessage", NULL,
                            "chat.db busy after 3 retries — Messages.app may be syncing");
            }
        }
        /* Log only when not yet circuit-broken; once tripped the breaker
         * already explained the situation in a single high-priority line. */
        if (!c->circuit_breaker_tripped)
            hu_log_error("imessage", NULL, "cannot open chat.db: error %d", rc);
        (void)imessage_record_open_result(c, rc, (int64_t)time(NULL));
        imessage_save_poll_status(c);
        return HU_ERR_IO;
    }
    /* Successful open resets the one-shot BUSY gate so a future episode
     * can emit one warn line again. */
    c->chatdb_busy_log_emitted = false;

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
                /* US-9.3: non-allowlisted DM — emit operator-side log every
                 * occurrence and (rate-limited) send a one-time courtesy
                 * reply explaining the allowlist requirement. */
                int64_t now_epoch = (int64_t)time(NULL);
                int64_t bucket = now_epoch / 86400;
                hu_log_info("imessage", NULL,
                            "non-allowlisted iMessage from %s; dropping (bucket=%lld)", handle,
                            (long long)bucket);
                bool dedup_already_replied = hu_imessage_courtesy_dedup_check(handle, bucket);
                uint32_t aggregate_today_count = hu_imessage_courtesy_aggregate_count(bucket);
                if (hu_imessage_should_courtesy_reply(false, dedup_already_replied,
                                                      c->courtesy_replies_enabled,
                                                      aggregate_today_count)) {
                    char reply[1024];
                    size_t reply_len = hu_imessage_build_courtesy_reply(NULL, c->default_target,
                                                                        reply, sizeof(reply));
                    if (reply_len > 0) {
                        (void)imessage_send(c, handle, handle_len, reply, reply_len, NULL, 0);
                        hu_imessage_courtesy_dedup_record(handle, bucket);
                    }
                }
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

/* ── Phase 2 Task 11 (RL SOTA): tapback inbound poll ─────────────────────
 *
 * hu_imessage_poll_reactions reads the chat.db `message` table for rows
 * whose associated_message_type sits in 2000-2006 (add) or 3000-3006
 * (remove), normalizes the code via hu_reaction_normalize_imessage (the
 * single source of truth for the tapback kind/polarity table — see
 * src/channels/reaction_event.c), and writes one hu_reaction_event_t per
 * row into the caller's buffer.
 *
 * The implementation lives in src/channels/imessage_reactions.c. It must
 * NOT live in this translation unit because human/channels/reaction_event.h
 * (Task 10) reuses two enumerator names already taken by the older
 * hu_reaction_type_t enum in human/channel.h (HU_REACTION_QUESTION,
 * HU_REACTION_CUSTOM_EMOJI). Including both headers in the same TU is a
 * compile error and Task 10 cannot be modified from Task 11. Splitting
 * the new function into its own .c (which only includes reaction_event.h)
 * sidesteps the conflict without touching either header. */

/* ── Tenor JSON fallback + GIF download (v2) ─────────────────────────── */

#if HU_IS_TEST || defined(HU_HTTP_CURL)
/* Simple JSON string extractor: find "key":"value" and return value.
 * Writes into out (up to cap). Returns length or 0 on failure. */
static size_t gif_json_extract(const char *json, size_t json_len, const char *key, char *out,
                               size_t cap) {
    size_t key_len = strlen(key);
    for (size_t i = 0; i + key_len + 3 < json_len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, key_len) == 0 &&
            json[i + 1 + key_len] == '"') {
            /* Skip ": or ":" */
            size_t j = i + 1 + key_len + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' '))
                j++;
            if (j < json_len && json[j] == '"') {
                j++;
                size_t start = j;
                while (j < json_len && json[j] != '"')
                    j++;
                size_t vlen = j - start;
                if (vlen >= cap)
                    vlen = cap - 1;
                memcpy(out, json + start, vlen);
                out[vlen] = '\0';
                return vlen;
            }
        }
    }
    return 0;
}
#endif /* HU_IS_TEST || HU_HTTP_CURL */

#if HU_IS_TEST
size_t hu_imessage_test_gif_json_extract(const char *json, size_t json_len, const char *key,
                                         char *out, size_t cap) {
    return gif_json_extract(json, json_len, key, out, cap);
}
#endif

#if !HU_IS_TEST && defined(HU_HTTP_CURL)
#include "human/core/http.h"
#include "human/core/json.h"

char *hu_imessage_fetch_gif(hu_allocator_t *alloc, const char *query, size_t query_len,
                            const char *api_key, size_t api_key_len) {
    if (!alloc || !query || query_len == 0 || !api_key || api_key_len == 0)
        return NULL;

    /* URL-encode the query: spaces to +, unreserved chars verbatim, rest %XX */
    char encoded[512];
    size_t eidx = 0;
    for (size_t i = 0; i < query_len && eidx + 3 < sizeof(encoded); i++) {
        unsigned char ch = (unsigned char)query[i];
        if (ch == ' ') {
            encoded[eidx++] = '+';
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded[eidx++] = (char)ch;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded[eidx++] = '%';
            encoded[eidx++] = hex[ch >> 4];
            encoded[eidx++] = hex[ch & 0x0F];
        }
    }
    encoded[eidx] = '\0';

    char url[512];
    int n = snprintf(url, sizeof(url),
                     "https://tenor.googleapis.com/v2/search?q=%s&key=%.*s"
                     "&client_key=human_app&limit=1&media_filter=gif",
                     encoded, (int)api_key_len, api_key);
    if (n < 0 || (size_t)n >= sizeof(url))
        return NULL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK || resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return NULL;
    }

    /* Extract the GIF URL from the JSON response.
     * Tenor v2 nests it at results[0].media_formats.gif.url */
    char gif_url[512];
    size_t gif_url_len = 0;

    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, resp.body, resp.body_len, &root) == HU_OK && root) {
        hu_json_value_t *results = hu_json_object_get(root, "results");
        hu_json_value_t *first = NULL;
        if (results && results->type == HU_JSON_ARRAY && results->data.array.len > 0)
            first = results->data.array.items[0];
        if (first && first->type == HU_JSON_OBJECT) {
            hu_json_value_t *media_formats = hu_json_object_get(first, "media_formats");
            if (media_formats && media_formats->type == HU_JSON_OBJECT) {
                hu_json_value_t *gif_obj = hu_json_object_get(media_formats, "gif");
                if (gif_obj && gif_obj->type == HU_JSON_OBJECT) {
                    const char *media_url = hu_json_get_string(gif_obj, "url");
                    if (media_url) {
                        size_t ulen = strlen(media_url);
                        if (ulen >= sizeof(gif_url))
                            ulen = sizeof(gif_url) - 1;
                        memcpy(gif_url, media_url, ulen);
                        gif_url[ulen] = '\0';
                        gif_url_len = ulen;
                    }
                }
            }
        }
        hu_json_free(alloc, root);
    }

    if (gif_url_len == 0) {
        /* Fallback if parse failed or response shape changed (field order, etc.) */
        const char *gif_section = NULL;
        for (size_t i = 0; i + 5 < resp.body_len; i++) {
            if (resp.body[i] == '"' && memcmp(resp.body + i, "\"gif\"", 5) == 0) {
                gif_section = resp.body + i;
                break;
            }
        }
        if (gif_section) {
            size_t remaining = resp.body_len - (size_t)(gif_section - resp.body);
            gif_url_len = gif_json_extract(gif_section, remaining, "url", gif_url, sizeof(gif_url));
        }
    }

    hu_http_response_free(alloc, &resp);

    if (gif_url_len == 0)
        return NULL;

    /* Download the GIF to a temp file */
    hu_http_response_t gif_resp = {0};
    err = hu_http_get(alloc, gif_url, NULL, &gif_resp);
    if (err != HU_OK || gif_resp.status_code != 200 || !gif_resp.body || gif_resp.body_len == 0) {
        if (gif_resp.owned && gif_resp.body)
            hu_http_response_free(alloc, &gif_resp);
        return NULL;
    }

    char tmp_path[256];
    static _Atomic unsigned gif_counter;
    unsigned gc = atomic_fetch_add(&gif_counter, 1);
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/human_gif_%u_%d_%u.gif", (unsigned)time(NULL),
             (int)getpid(), gc);

    /* /tmp GIF download buffer — short-lived, but still goes through
     * hu_io_secure_open so the path is checked for traversal and the
     * mode is explicit (0644 — GIF is shown to the user via the
     * channel). */
    FILE *f = NULL;
    if (hu_io_secure_open(tmp_path, HU_IO_PERM_USER, "wb", &f) != HU_OK || !f) {
        hu_http_response_free(alloc, &gif_resp);
        return NULL;
    }
    fwrite(gif_resp.body, 1, gif_resp.body_len, f);
    fclose(f);
    hu_http_response_free(alloc, &gif_resp);

    size_t path_len = strlen(tmp_path);
    char *result = (char *)alloc->alloc(alloc->ctx, path_len + 1);
    if (!result)
        return NULL;
    memcpy(result, tmp_path, path_len + 1);
    return result;
}
#else
char *hu_imessage_fetch_gif(hu_allocator_t *alloc, const char *query, size_t query_len,
                            const char *api_key, size_t api_key_len) {
    (void)alloc;
    (void)query;
    (void)query_len;
    (void)api_key;
    (void)api_key_len;
    return NULL;
}
#endif

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

/* ── Onboarding wizard support ─────────────────────────────────────────── */

hu_error_t hu_imessage_detect_self_handle(hu_allocator_t *alloc, char *buf, size_t buf_size) {
    (void)alloc;

    if (!buf || buf_size == 0)
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(__APPLE__) || HU_IS_TEST
    return HU_ERR_NOT_FOUND;
#else
#ifdef HU_ENABLE_SQLITE
    const char *home = getenv("HOME");
    if (!home) {
        buf[0] = '\0';
        return HU_ERR_NOT_FOUND;
    }

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= sizeof(db_path)) {
        buf[0] = '\0';
        return HU_ERR_IO;
    }

    sqlite3 *db = NULL;
    int rc = imessage_open_chatdb(db_path, &db);
    if (rc != SQLITE_OK || !db) {
        buf[0] = '\0';
        return HU_ERR_NOT_FOUND;
    }

    /* Query the handle table for the "Me" record. In iMessage's SQLite schema,
     * the handle table contains contact info; the "Me" record is typically
     * the first row or can be identified by specific criteria. We query by
     * looking for the handle with a known pattern or by scanning the rows. */
    const char *sql = "SELECT id FROM handle LIMIT 1";

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        buf[0] = '\0';
        return HU_ERR_IO;
    }

    hu_error_t result = HU_ERR_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *handle = (const char *)sqlite3_column_text(stmt, 0);
        if (handle) {
            size_t handle_len = strlen(handle);
            if (handle_len > 0 && handle_len < buf_size) {
                memcpy(buf, handle, handle_len);
                buf[handle_len] = '\0';
                result = HU_OK;
            } else {
                buf[0] = '\0';
                result = HU_ERR_IO; /* Buffer too small */
            }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
#else
    buf[0] = '\0';
    return HU_ERR_NOT_FOUND;
#endif /* HU_ENABLE_SQLITE */
#endif /* __APPLE__ && !HU_IS_TEST */
}

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

const char *hu_imessage_test_get_last_courtesy_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_courtesy_message_len;
    return c->last_courtesy_message;
}

void hu_imessage_test_clear_last_courtesy_message(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    c->last_courtesy_message[0] = '\0';
    c->last_courtesy_message_len = 0;
}

/* Test-only: drive the non-allowlisted-handle pipeline without needing a
 * real chat.db. Calls the same predicate + dedup + builder used by the
 * production poll loop (and uses the channel's own .send vtable entry, so
 * the echo ring is populated as it would be in prod). */
hu_error_t hu_imessage_test_handle_non_allowlisted(hu_channel_t *ch, const char *handle,
                                                   int64_t mock_epoch) {
    if (!ch || !ch->ctx || !handle || !handle[0])
        return HU_ERR_INVALID_ARGUMENT;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    int64_t bucket = mock_epoch / 86400;
    bool dedup_already_replied = hu_imessage_courtesy_dedup_check(handle, bucket);
    uint32_t aggregate_today_count = hu_imessage_courtesy_aggregate_count(bucket);
    /* Operator-side visibility: log every non-allowlisted DM (per design),
     * regardless of whether we send a reply. Uses info-level so it does
     * not trip log-noise alerts. */
    hu_log_info("imessage", NULL, "non-allowlisted iMessage from %s; dropping (bucket=%lld)",
                handle, (long long)bucket);
    bool should = hu_imessage_should_courtesy_reply(
        false, dedup_already_replied, c->courtesy_replies_enabled, aggregate_today_count);
    if (!should)
        return HU_OK;
    char reply[1024];
    size_t reply_len = hu_imessage_build_courtesy_reply(NULL, NULL, reply, sizeof(reply));
    if (reply_len == 0)
        return HU_OK; /* defensive */
    /* Send through the same path the production poll loop uses (static
     * imessage_send in this TU). This populates the echo ring and, under
     * HU_IS_TEST, records to c->last_message. */
    (void)imessage_send(c, handle, strlen(handle), reply, reply_len, NULL, 0);
    /* Mirror to the test-only courtesy field. */
    size_t mirror = reply_len < sizeof(c->last_courtesy_message) - 1
                        ? reply_len
                        : sizeof(c->last_courtesy_message) - 1;
    memcpy(c->last_courtesy_message, reply, mirror);
    c->last_courtesy_message[mirror] = '\0';
    c->last_courtesy_message_len = mirror;
    /* Record AFTER successful send (best-effort; we already sent). */
    hu_imessage_courtesy_dedup_record(handle, bucket);
    return HU_OK;
}

void hu_imessage_test_record_chatdb_busy_exhaustion(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    /* Mirror the production AC-9.3.3 handler exactly: persist poll status
     * immediately AND emit a one-shot warn line. */
    imessage_save_poll_status(c);
    if (!c->chatdb_busy_log_emitted) {
        c->chatdb_busy_log_emitted = true;
        hu_log_warn("imessage", NULL, "chat.db busy after 3 retries — Messages.app may be syncing");
    }
}

void hu_imessage_test_reset_chatdb_busy_gate(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    c->chatdb_busy_log_emitted = false;
}

bool hu_imessage_test_chatdb_busy_log_emitted(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_imessage_ctx_t *c = (hu_imessage_ctx_t *)ch->ctx;
    return c->chatdb_busy_log_emitted;
}

void hu_imessage_set_test_send_stub(hu_imessage_test_send_stub_fn fn) {
    g_imessage_test_send_stub = fn;
}
#endif
