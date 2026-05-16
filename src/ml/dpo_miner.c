/* dpo_miner — see include/human/ml/dpo_miner.h for the contract.
 *
 * Sprint 7 US-7.2 (Option (b) per D2). The miner walks chat.db's
 * `messages` table looking for `user -> assistant -> user` triples within
 * `correction_window_sec`, treats each triple as a preference signal
 * (rejected = agent's response, chosen = user's correcting followup,
 * prompt = original user message), redacts PII, and records into
 * `dpo_pairs` after a content-hash dedup check.
 *
 * Compile-time gating: SQLite is required. Without HU_ENABLE_SQLITE the
 * function returns HU_ERR_NOT_SUPPORTED.
 */

#include "human/ml/dpo_miner.h"
#include "human/ml/dpo.h"
#include "human/ml/training_data_extractor.h"
#include "human/ml/training_data_quality.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Public test seam — see header. Resolves the should-export / export-path
 * decision from the parsed CLI flag state. */
hu_error_t hu_dpo_miner_resolve_export(const char *export_flag_path, int no_export_flag_set,
                                       const char *home_dir, hu_dpo_miner_export_decision_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* HIGH-1 contract: --no-export is the highest-priority signal that
     * overrides any default-enable. */
    if (no_export_flag_set) {
        out->should_export = 0;
        return HU_OK;
    }

    if (export_flag_path && export_flag_path[0]) {
        int n = snprintf(out->export_path, sizeof(out->export_path), "%s", export_flag_path);
        if (n <= 0 || (size_t)n >= sizeof(out->export_path))
            return HU_ERR_INTERNAL;
        out->should_export = 1;
        return HU_OK;
    }

    /* Default-enable: cross-story lock with US-7.1. Path requires HOME. */
    if (!home_dir || !home_dir[0]) {
        out->should_export = 0;
        return HU_OK;
    }
    int n =
        snprintf(out->export_path, sizeof(out->export_path), "%s/.human/dpo/pairs.jsonl", home_dir);
    if (n <= 0 || (size_t)n >= sizeof(out->export_path))
        return HU_ERR_INTERNAL;
    out->should_export = 1;
    return HU_OK;
}

/* Public test seam — see header. Creates the parent directory of
 * `file_path` (and any intermediate components) if missing. Uses mode
 * 0700 to match the secrets-class permission applied by `hu_io_secure_open`
 * when writing pairs.jsonl. POSIX-only; on other platforms returns HU_OK
 * without trying. */
hu_error_t hu_dpo_miner_ensure_parent_dir(const char *file_path) {
    if (!file_path || !file_path[0])
        return HU_ERR_INVALID_ARGUMENT;

    /* Find the last '/' — anything before it is the parent dir. If there
     * is no slash, the file is in the current dir and no mkdir is needed. */
    const char *last_slash = strrchr(file_path, '/');
    if (!last_slash || last_slash == file_path)
        return HU_OK;

    size_t parent_len = (size_t)(last_slash - file_path);
    if (parent_len == 0 || parent_len >= 1024)
        return HU_ERR_INVALID_ARGUMENT;

    char parent[1024];
    memcpy(parent, file_path, parent_len);
    parent[parent_len] = '\0';

    /* Walk components left-to-right, creating each prefix as needed. Skip
     * the leading '/' so we always have a non-empty path to mkdir. */
    for (size_t i = 1; i <= parent_len; i++) {
        if (i == parent_len || parent[i] == '/') {
            char saved = parent[i];
            parent[i] = '\0';
            if (mkdir(parent, 0700) != 0 && errno != EEXIST) {
                /* Some platforms report EISDIR for an existing dir with
                 * an unusual entry type; tolerate it the same as EEXIST. */
                if (errno != EISDIR) {
                    parent[i] = saved;
                    return HU_ERR_IO;
                }
            }
            parent[i] = saved;
        }
    }
    return HU_OK;
}

#ifdef HU_ENABLE_SQLITE

/* FNV-1a 64-bit. Matches the family of hash used by the imessage dedup
 * ring (32-bit there; 64-bit here to drop collision risk on dpo_pair_hashes
 * across many runs). */
static uint64_t dpo_miner_hash64(const char *s, size_t len) {
    uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
    for (size_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ULL; /* FNV prime */
    return h;
}

/* CREATE the dedup-tracking table. Triple primary key matches the AC-7.2.5
 * contract: same (prompt, chosen, rejected) hash triple is never recorded
 * twice. Stored as INTEGER (sqlite holds int64 natively). */
static hu_error_t ensure_pair_hashes_table(sqlite3 *db) {
    const char *sql = "CREATE TABLE IF NOT EXISTS dpo_pair_hashes("
                      "prompt_hash INTEGER NOT NULL, "
                      "chosen_hash INTEGER NOT NULL, "
                      "rejected_hash INTEGER NOT NULL, "
                      "recorded_at INTEGER, "
                      "PRIMARY KEY(prompt_hash, chosen_hash, rejected_hash))";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* CREATE the dpo_pairs table (mirrors hu_dpo_init_tables exactly). The
 * miner cannot rely on the caller having created it. */
static hu_error_t ensure_dpo_pairs_table(sqlite3 *db) {
    const char *sql = "CREATE TABLE IF NOT EXISTS dpo_pairs("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "prompt TEXT, chosen TEXT, rejected TEXT, "
                      "margin REAL, timestamp INTEGER, source TEXT)";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg)
            sqlite3_free(err_msg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* Returns 1 if a row with this triple of hashes already exists in
 * dpo_pair_hashes, 0 if not, -1 on error. */
static int hash_triple_seen(sqlite3 *db, uint64_t ph, uint64_t ch, uint64_t rh) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT 1 FROM dpo_pair_hashes "
                      "WHERE prompt_hash = ? AND chosen_hash = ? AND rejected_hash = ? LIMIT 1";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)ph);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)ch);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)rh);
    int seen = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        seen = 1;
    sqlite3_finalize(stmt);
    return seen;
}

/* Insert the (prompt, chosen, rejected) hash triple into the tracking
 * table. INSERT OR IGNORE handles the race-with-self case if another
 * code path also tries to record the same triple concurrently. */
static hu_error_t record_hash_triple(sqlite3 *db, uint64_t ph, uint64_t ch, uint64_t rh) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO dpo_pair_hashes"
                      "(prompt_hash, chosen_hash, rejected_hash, recorded_at) "
                      "VALUES(?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)ph);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)ch);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)rh);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    return HU_OK;
}

/* Redact `src` (NUL-terminated, length cached) into `out` using
 * `hu_pii_redact`. Updates `*total_redactions` with the count from this
 * call. Returns the post-redaction byte length (excluding NUL) or
 * (size_t)-1 on failure. */
static size_t pii_redact_field(const char *src, size_t src_len, char *out, size_t out_cap,
                               size_t *total_redactions) {
    if (!src || !out || out_cap == 0)
        return (size_t)-1;
    hu_pii_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    size_t redacted_len = 0;
    hu_error_t err = hu_pii_redact(src, src_len, out, out_cap, &redacted_len, &stats);
    if (err != HU_OK)
        return (size_t)-1;
    if (total_redactions)
        *total_redactions += (size_t)hu_pii_total(&stats);
    return redacted_len;
}

hu_error_t hu_dpo_mine_corrections(hu_allocator_t *alloc, sqlite3 *db,
                                   const hu_dpo_mine_opts_t *opts, hu_dpo_mine_stats_t *stats) {
    if (!alloc || !db)
        return HU_ERR_INVALID_ARGUMENT;

    int correction_window_sec = HU_DPO_CORRECTION_WINDOW_SEC;
    size_t max_rows = 1000; /* matches the LIMIT used by the existing extractor */
    if (opts) {
        if (opts->correction_window_sec > 0)
            correction_window_sec = opts->correction_window_sec;
        if (opts->max_rows > 0)
            max_rows = opts->max_rows;
    }

    hu_dpo_mine_stats_t local_stats;
    memset(&local_stats, 0, sizeof(local_stats));

    /* Ensure both write tables exist before doing anything else. */
    hu_error_t err = ensure_dpo_pairs_table(db);
    if (err != HU_OK)
        return err;
    err = ensure_pair_hashes_table(db);
    if (err != HU_OK)
        return err;

    /* Set up a collector that wraps the same DB. The collector's
     * `max_pairs` is a generous cap — the eviction step inside
     * `hu_dpo_record_pair` is a no-op when total stays under it. */
    hu_dpo_collector_t collector;
    err = hu_dpo_collector_create(alloc, db, 100000, &collector);
    if (err != HU_OK)
        return err;

    /* 3-turn correction SELECT — semantics mirror
     * `hu_training_data_extract_dpo` (src/ml/training_data_extractor.c).
     * The miner deliberately does NOT join against
     * `dpo_auto_extractions` — that table is owned by the other
     * extractor, and conflating tracking tables would make a `--reset`
     * on either path corrupt the other. The miner's de-dupe lives in
     * dpo_pair_hashes via content hash. */
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT u1.content, a.content, u2.content "
                      "FROM messages a "
                      "JOIN messages u1 ON u1.session_id = a.session_id "
                      "  AND u1.role = 'user' AND u1.id = ("
                      "    SELECT MAX(id) FROM messages "
                      "    WHERE session_id = a.session_id AND role = 'user' AND id < a.id"
                      "  ) "
                      "JOIN messages u2 ON u2.session_id = a.session_id "
                      "  AND u2.role = 'user' AND u2.id = ("
                      "    SELECT MIN(id) FROM messages "
                      "    WHERE session_id = a.session_id AND role = 'user' AND id > a.id"
                      "  ) "
                      "WHERE a.role = 'assistant' "
                      "AND (julianday(u2.created_at) - julianday(a.created_at)) * 86400 <= ? "
                      "AND (julianday(u2.created_at) - julianday(a.created_at)) * 86400 > 0 "
                      "ORDER BY a.id ASC "
                      "LIMIT ?";

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        hu_dpo_collector_deinit(&collector);
        return HU_ERR_IO;
    }
    sqlite3_bind_int(stmt, 1, correction_window_sec);
    sqlite3_bind_int(stmt, 2, (int)max_rows);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        local_stats.triples_examined++;

        const char *prompt = (const char *)sqlite3_column_text(stmt, 0);
        const char *rejected = (const char *)sqlite3_column_text(stmt, 1);
        const char *chosen = (const char *)sqlite3_column_text(stmt, 2);
        if (!prompt || !rejected || !chosen)
            continue;
        if (!prompt[0] || !rejected[0] || !chosen[0])
            continue;

        size_t prompt_src_len = strlen(prompt);
        size_t rejected_src_len = strlen(rejected);
        size_t chosen_src_len = strlen(chosen);

        /* Redaction buffers sized per hu_pii_redact contract:
         * out_cap >= text_len + 16 worst case. We size to fit the
         * hu_preference_pair_t field caps so over-cap inputs are
         * detected by the "redacted_len > cap" check that follows. */
        char redacted_prompt[2048 + 64];
        char redacted_rejected[4096 + 64];
        char redacted_chosen[4096 + 64];

        size_t rp_len = pii_redact_field(prompt, prompt_src_len, redacted_prompt,
                                         sizeof(redacted_prompt), &local_stats.pii_redactions);
        size_t rr_len = pii_redact_field(rejected, rejected_src_len, redacted_rejected,
                                         sizeof(redacted_rejected), &local_stats.pii_redactions);
        size_t rc_len = pii_redact_field(chosen, chosen_src_len, redacted_chosen,
                                         sizeof(redacted_chosen), &local_stats.pii_redactions);
        if (rp_len == (size_t)-1 || rr_len == (size_t)-1 || rc_len == (size_t)-1)
            continue;

        /* Hard cap to hu_preference_pair_t field sizes (minus one for NUL).
         * Per design §4.2 we drop the pair entirely if any field overflows;
         * a truncated training example does more harm than dropping it. */
        if (rp_len >= 2048 || rr_len >= 4096 || rc_len >= 4096) {
            local_stats.pairs_skipped_size++;
            continue;
        }

        /* Hash each redacted field. Dedup is content-keyed (after PII
         * redaction) so two inputs that redact identically count as the
         * same correction — desirable for AC-7.2.5. */
        uint64_t ph = dpo_miner_hash64(redacted_prompt, rp_len);
        uint64_t rh = dpo_miner_hash64(redacted_rejected, rr_len);
        uint64_t ch = dpo_miner_hash64(redacted_chosen, rc_len);

        int seen = hash_triple_seen(db, ph, ch, rh);
        if (seen < 0) {
            /* DB error on dedup check — bail rather than silently
             * inserting (would defeat AC-7.2.5). */
            sqlite3_finalize(stmt);
            hu_dpo_collector_deinit(&collector);
            return HU_ERR_IO;
        }
        if (seen) {
            local_stats.pairs_skipped_dup++;
            continue;
        }

        /* Build the preference pair. source = "outbound_edit" per
         * AC-7.2.1; margin = 0.5 per AC-7.2.1. */
        hu_preference_pair_t pair;
        memset(&pair, 0, sizeof(pair));
        memcpy(pair.prompt, redacted_prompt, rp_len);
        pair.prompt_len = rp_len;
        memcpy(pair.chosen, redacted_chosen, rc_len);
        pair.chosen_len = rc_len;
        memcpy(pair.rejected, redacted_rejected, rr_len);
        pair.rejected_len = rr_len;
        pair.margin = 0.5;
        pair.timestamp = (int64_t)time(NULL);
        const char *src_tag = "outbound_edit";
        size_t src_tag_len = strlen(src_tag);
        memcpy(pair.source, src_tag, src_tag_len);
        pair.source_len = src_tag_len;

        err = hu_dpo_record_pair(&collector, &pair);
        if (err != HU_OK)
            continue;

        /* Only stamp the hash table once the pair landed — otherwise a
         * failed insert would block legitimate retries on the next run. */
        err = record_hash_triple(db, ph, ch, rh);
        if (err != HU_OK) {
            /* Pair was recorded but the hash didn't stamp. Surface the
             * inconsistency — better than silently producing duplicates
             * on the next run. */
            sqlite3_finalize(stmt);
            hu_dpo_collector_deinit(&collector);
            return err;
        }
        local_stats.pairs_recorded++;
    }
    sqlite3_finalize(stmt);
    hu_dpo_collector_deinit(&collector);

    if (stats)
        *stats = local_stats;
    return HU_OK;
}

#else /* HU_ENABLE_SQLITE */

hu_error_t hu_dpo_mine_corrections(hu_allocator_t *alloc, void *db, const hu_dpo_mine_opts_t *opts,
                                   hu_dpo_mine_stats_t *stats) {
    (void)alloc;
    (void)db;
    (void)opts;
    (void)stats;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
