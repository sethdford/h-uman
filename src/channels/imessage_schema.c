/* src/channels/imessage_schema.c
 *
 * Phase 6 of docs/plans/2026-05-18-imessage-sota.md: schema-version-aware
 * probe + drift canary for chat.db.
 *
 * Why this module exists:
 *   Existing iMessage readers do ad-hoc runtime fallback (e.g.
 *   imessage_reactions.c probes for sql_v17 with associated_message_emoji,
 *   falling back to sql_legacy on error). This pattern is opaque, repeats
 *   per reader, and never alerts when Apple adds a column we don't know
 *   about. This module replaces that with one centralized probe.
 *
 * What it does:
 *   PRAGMA table_info(message) → iterate column names → classify each
 *   against a known-set → unknown columns get captured for drift alerting
 *   → SHA-256 of sorted column names becomes a stable fingerprint.
 *
 * What it does NOT do (Phase 6 scope discipline):
 *   - It does not refactor existing readers to consult the cached caps.
 *     That's a follow-up sprint per the plan; touching every call site
 *     here would violate ~/.claude/rules/agent-task-sizing.md and the
 *     Phase 6 instructions. */

#include "human/channels/imessage_schema.h"
#include "human/core/log.h"
#include "human/crypto.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

/* ---------- known column set --------------------------------------------- */

/* The schema-classification helpers below (known-column set, column vector,
 * fingerprint, per-column classifier) are referenced only from the SQLite
 * probe path (hu_imessage_schema_probe's #else branch). In builds without
 * HU_ENABLE_SQLITE the probe short-circuits to HU_ERR_NOT_SUPPORTED, so
 * these would be unused (-Werror=unused-function/-variable). Gate them. */
#if defined(HU_ENABLE_SQLITE)

/* Columns we explicitly recognize as part of the canonical `message` table
 * across the macOS versions we support. A column present in chat.db but NOT
 * in this list goes into unknown_columns[] as a drift canary entry.
 *
 * Sources: ~/Library/Messages/chat.db PRAGMA table_info(message) on
 * macOS 10.15 Catalina through macOS 15 Sequoia. */
static const char *const KNOWN_MESSAGE_COLUMNS[] = {
    /* Catalina-era core */
    "ROWID",
    "guid",
    "text",
    "replace",
    "service_center",
    "handle_id",
    "subject",
    "country",
    "attributedBody",
    "version",
    "type",
    "service",
    "account",
    "account_guid",
    "error",
    "date",
    "date_read",
    "date_delivered",
    "is_delivered",
    "is_finished",
    "is_emote",
    "is_from_me",
    "is_empty",
    "is_delayed",
    "is_auto_reply",
    "is_prepared",
    "is_read",
    "is_system_message",
    "is_sent",
    "has_dd_results",
    "is_service_message",
    "is_forward",
    "was_downgraded",
    "is_archive",
    "cache_has_attachments",
    "cache_roomnames",
    "was_data_detected",
    "was_deduplicated",
    "is_audio_message",
    "is_played",
    "date_played",
    "item_type",
    "other_handle",
    "group_title",
    "group_action_type",
    "share_status",
    "share_direction",
    "is_expirable",
    "expire_state",
    "message_action_type",
    "message_source",
    "associated_message_guid",
    "associated_message_type",
    "balloon_bundle_id",
    "payload_data",
    "expressive_send_style_id",
    "associated_message_range_location",
    "associated_message_range_length",
    "time_expressive_send_played",
    "message_summary_info",
    "ck_sync_state",
    "ck_record_id",
    "ck_record_change_tag",
    "destination_caller_id",
    "is_corrupt",
    "reply_to_guid",
    "sort_id",
    "is_spam",
    "has_unseen_mention",
    /* Ventura+ */
    "thread_originator_guid",
    "thread_originator_part",
    "syndication_ranges",
    "synced_syndication_ranges",
    "was_delivered_quietly",
    "did_notify_recipient",
    "date_retracted",
    "date_edited",
    "was_detonated",
    "part_count",
    "is_stewie",
    /* Sonoma+ */
    "associated_message_emoji",
    NULL,
};

static bool is_known_column(const char *name) {
    if (!name)
        return false;
    for (size_t i = 0; KNOWN_MESSAGE_COLUMNS[i]; i++) {
        if (strcmp(name, KNOWN_MESSAGE_COLUMNS[i]) == 0)
            return true;
    }
    return false;
}

/* ---------- column-name vector + sort + fingerprint ---------------------- */

#define HU_SCHEMA_MAX_COLUMNS 256
#define HU_SCHEMA_NAME_BUF    64

typedef struct {
    char names[HU_SCHEMA_MAX_COLUMNS][HU_SCHEMA_NAME_BUF];
    size_t n;
} column_vec_t;

static int cmp_str_ptr(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void compute_fingerprint(column_vec_t *cols,
                                char out_hex[HU_IMESSAGE_SCHEMA_FINGERPRINT_HEX_LEN]) {
    /* Sort in-place so the fingerprint is order-stable. PRAGMA table_info
     * already returns cid-ordered, which matches the order columns were
     * declared — that's stable PER schema, but we want the fingerprint to
     * be order-independent so renaming a CREATE TABLE statement that
     * reorders unchanged columns doesn't churn the fingerprint. */
    qsort(cols->names, cols->n, HU_SCHEMA_NAME_BUF, cmp_str_ptr);

    /* SHA-256 over "name1\nname2\n..." */
    /* Compute total length first to allocate a single contiguous buffer. */
    size_t total = 0;
    for (size_t i = 0; i < cols->n; i++)
        total += strlen(cols->names[i]) + 1; /* +1 for '\n' */
    if (total == 0) {
        memset(out_hex, '0', 64);
        out_hex[64] = '\0';
        return;
    }
    char *buf = (char *)malloc(total);
    if (!buf) {
        memset(out_hex, '0', 64);
        out_hex[64] = '\0';
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < cols->n; i++) {
        size_t l = strlen(cols->names[i]);
        memcpy(buf + pos, cols->names[i], l);
        pos += l;
        buf[pos++] = '\n';
    }

    uint8_t digest[32];
    hu_sha256((const uint8_t *)buf, pos, digest);
    free(buf);

    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out_hex[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

/* ---------- caps classification ------------------------------------------ */

static void classify_column(hu_imessage_schema_caps_t *caps, const char *name) {
    if (strcmp(name, "date_retracted") == 0)
        caps->has_date_retracted = true;
    else if (strcmp(name, "thread_originator_guid") == 0)
        caps->has_thread_originator_guid = true;
    else if (strcmp(name, "associated_message_emoji") == 0)
        caps->has_associated_message_emoji = true;
    else if (strcmp(name, "group_action_type") == 0)
        caps->has_group_action_type = true;
    else if (strcmp(name, "group_title") == 0)
        caps->has_group_title = true;
    else if (strcmp(name, "balloon_bundle_id") == 0)
        caps->has_balloon_bundle_id = true;
    else if (strcmp(name, "expressive_send_style_id") == 0)
        caps->has_expressive_send_style_id = true;
    else if (strcmp(name, "payload_data") == 0)
        caps->has_payload_data = true;
    else if (strcmp(name, "message_summary_info") == 0)
        caps->has_message_summary_info = true;

    if (!is_known_column(name)) {
        if (caps->unknown_column_count < HU_IMESSAGE_SCHEMA_UNKNOWN_MAX) {
            size_t i = caps->unknown_column_count++;
            strncpy(caps->unknown_columns[i], name, HU_IMESSAGE_SCHEMA_UNKNOWN_NAME_MAX - 1);
            caps->unknown_columns[i][HU_IMESSAGE_SCHEMA_UNKNOWN_NAME_MAX - 1] = '\0';
        }
    }
}

#endif /* HU_ENABLE_SQLITE — schema-classification helpers */

/* ---------- cache -------------------------------------------------------- */

/* Single-slot cache: keyed by db_path. Phase 6 is a daemon with one chat.db;
 * a future multi-path use would replace this with a small map. */
typedef struct {
    bool valid;
    char path[512];
    hu_imessage_schema_caps_t caps;
} cache_slot_t;

static cache_slot_t g_cache;

void hu_imessage_schema_reset_cache(void) {
    memset(&g_cache, 0, sizeof(g_cache));
}

/* ---------- probe -------------------------------------------------------- */

hu_error_t hu_imessage_schema_probe(const char *db_path, hu_imessage_schema_caps_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;

    /* 2026-05-26 fix: sqlite3_open_v2 doesn't expand `~` — that's a
     * shell construct. The original default `"~/Library/Messages/chat.db"`
     * tried to open a file literally named `~/Library/...` which never
     * exists, so the probe ALWAYS returned HU_ERR_IO on macOS. Operators
     * never saw the schema fingerprint log; every startup fell to "blind
     * queries" mode silently. Match the pattern used by the other
     * readers (imessage.c:878, :1121, :2005, :2193, :2300, :2356) and
     * resolve via $HOME explicitly.
     *
     * `default_path` is on the stack — sized for the longest realistic
     * $HOME path; if $HOME is unset we fall back to the literal tilde
     * which the existing log message will surface as an IO error, the
     * same operator-visible failure mode as before. */
    char default_path[512];
    const char *resolved = db_path;
    if (!resolved) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            int n =
                snprintf(default_path, sizeof(default_path), "%s/Library/Messages/chat.db", home);
            if (n > 0 && (size_t)n < sizeof(default_path))
                resolved = default_path;
        }
        if (!resolved)
            resolved = "~/Library/Messages/chat.db"; /* surface as IO error */
    }
    const char *path = resolved;

    /* Cache hit short-circuit */
    if (g_cache.valid && strncmp(g_cache.path, path, sizeof(g_cache.path)) == 0) {
        *out = g_cache.caps;
        return HU_OK;
    }

#if !defined(HU_ENABLE_SQLITE)
    (void)path;
    memset(out, 0, sizeof(*out));
    return HU_ERR_NOT_SUPPORTED;
#else
    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA table_info(message)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    column_vec_t cols;
    cols.n = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        /* PRAGMA table_info columns: cid|name|type|notnull|dflt_value|pk */
        const unsigned char *col_name = sqlite3_column_text(stmt, 1);
        if (!col_name)
            continue;
        const char *name = (const char *)col_name;

        classify_column(out, name);

        if (cols.n < HU_SCHEMA_MAX_COLUMNS) {
            strncpy(cols.names[cols.n], name, HU_SCHEMA_NAME_BUF - 1);
            cols.names[cols.n][HU_SCHEMA_NAME_BUF - 1] = '\0';
            cols.n++;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (cols.n == 0) {
        /* table_info returned no rows — the `message` table doesn't exist
         * or chat.db is empty. That's an IO-level failure: the caller's
         * chat.db isn't a real iMessage database. */
        return HU_ERR_IO;
    }

    compute_fingerprint(&cols, out->schema_fingerprint);
    out->probed = true;

    /* Cache the result. */
    g_cache.valid = true;
    strncpy(g_cache.path, path, sizeof(g_cache.path) - 1);
    g_cache.path[sizeof(g_cache.path) - 1] = '\0';
    g_cache.caps = *out;

    return HU_OK;
#endif
}

/* ---------- logging ------------------------------------------------------ */

void hu_imessage_schema_log_fingerprint(const hu_imessage_schema_caps_t *caps) {
    if (!caps || !caps->probed) {
        hu_log_warn("imessage", NULL,
                    "schema probe not performed; chat.db reads may use blind fallback");
        return;
    }

    hu_log_info("imessage", NULL,
                "schema fingerprint=%.16s... (caps: tapback_emoji=%s edit_history=%s "
                "group_events=%s sticker=%s)",
                caps->schema_fingerprint, caps->has_associated_message_emoji ? "yes" : "no",
                caps->has_date_retracted ? "yes" : "no", caps->has_group_action_type ? "yes" : "no",
                caps->has_balloon_bundle_id ? "yes" : "no");

    if (caps->unknown_column_count > 0) {
        /* Build a comma-joined list of unknowns up to ~400 chars. */
        char buf[512];
        size_t pos = 0;
        for (size_t i = 0; i < caps->unknown_column_count && pos + 64 < sizeof(buf); i++) {
            int written = snprintf(buf + pos, sizeof(buf) - pos, "%s%s", i == 0 ? "" : ",",
                                   caps->unknown_columns[i]);
            if (written < 0)
                break;
            pos += (size_t)written;
        }
        buf[sizeof(buf) - 1] = '\0';
        hu_log_warn("imessage", NULL,
                    "schema drift: %zu unknown column(s) in chat.db `message` table: %s "
                    "— Apple may have added fields this build doesn't recognize",
                    caps->unknown_column_count, buf);
    }
}
