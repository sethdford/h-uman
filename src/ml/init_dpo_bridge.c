/* src/ml/init_dpo_bridge.c
 *
 * Bridge from init_outcome resolutions → dpo_pairs single-sided rows.
 *
 * Lifecycle:
 *   daemon init → hu_dpo_collector_create → hu_init_dpo_bridge_set_collector
 *   resolver tick → hu_init_outcome_resolve_pending → bridge record (per
 *                   resolution) → hu_dpo_record_pair → SQLite INSERT
 *   daemon shutdown → hu_dpo_collector_deinit (bridge holds borrow only)
 *
 * See include/human/ml/init_dpo_bridge.h for the contract.
 */

#ifdef HU_ENABLE_ML

#include "human/ml/init_dpo_bridge.h"
#include "human/ml/cli.h"
#include "human/ml/dpo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* Module-private collector pointer. Borrowed — daemon owns the underlying
 * struct + DB handle. Set/cleared via hu_init_dpo_bridge_set_collector. */
static struct hu_dpo_collector *s_collector = NULL;

void hu_init_dpo_bridge_set_collector(struct hu_dpo_collector *collector) {
    s_collector = collector;
}

struct hu_dpo_collector *hu_init_dpo_bridge_get_collector(void) {
    return s_collector;
}

bool hu_init_dpo_bridge_should_pair_now(size_t resolutions_since_last, size_t threshold) {
    if (threshold == 0)
        return false; /* operator disabled auto-pair */
    return resolutions_since_last >= threshold;
}

hu_error_t hu_init_dpo_bridge_record(hu_allocator_t *alloc, hu_init_resolution_t outcome,
                                     const char *draft, const char *target, int64_t resolution_ts) {
    (void)alloc; /* unused — hu_dpo_record_pair holds its own alloc handle */

    if (!draft)
        return HU_ERR_INVALID_ARGUMENT;
    if (outcome != HU_INIT_RESOLUTION_REPLIED && outcome != HU_INIT_RESOLUTION_IGNORED)
        return HU_ERR_INVALID_ARGUMENT;
    if (!s_collector)
        return HU_ERR_NOT_SUPPORTED;

    /* Build the preference pair. Prompt template is intentionally
     * minimal-but-stable: a downstream pairing pass that wants richer
     * context can JOIN on (timestamp, source) to grab the matching
     * init_outcome JSONL line. Keeping the prompt small here also
     * leaves room for chosen/rejected within the 4096-byte field. */
    hu_preference_pair_t pair;
    memset(&pair, 0, sizeof(pair));

    /* prompt: a short identifier that lets readers locate the row's
     * context without parsing free text. Format:
     *   "proactive-proposal: target=<handle> ts=<unix>"
     * Stable, machine-grep-able, and survives schema growth. */
    const char *safe_target = (target && target[0]) ? target : "unknown";
    int pn = snprintf(pair.prompt, sizeof(pair.prompt), "proactive-proposal: target=%s ts=%lld",
                      safe_target, (long long)resolution_ts);
    if (pn < 0)
        return HU_ERR_IO;
    pair.prompt_len = (size_t)pn < sizeof(pair.prompt) ? (size_t)pn : sizeof(pair.prompt) - 1;

    /* Single-sided per outcome. Truncate at field-cap defensively
     * (pending_proposal_t.draft is 1024, chosen/rejected are 4096, so
     * truncation should never bite — defensive belt). */
    size_t draft_len = strlen(draft);
    if (outcome == HU_INIT_RESOLUTION_REPLIED) {
        size_t copy = draft_len < sizeof(pair.chosen) - 1 ? draft_len : sizeof(pair.chosen) - 1;
        memcpy(pair.chosen, draft, copy);
        pair.chosen[copy] = '\0';
        pair.chosen_len = copy;
        pair.rejected[0] = '\0';
        pair.rejected_len = 0;
    } else { /* IGNORED */
        size_t copy = draft_len < sizeof(pair.rejected) - 1 ? draft_len : sizeof(pair.rejected) - 1;
        memcpy(pair.rejected, draft, copy);
        pair.rejected[copy] = '\0';
        pair.rejected_len = copy;
        pair.chosen[0] = '\0';
        pair.chosen_len = 0;
    }

    /* margin=1.0 for both single-sided cases. A future pairing pass can
     * derive a real margin from confidence-at-fire-time and reply
     * latency; until then, 1.0 is "we observed the outcome with full
     * certainty" (no LLM judgment was involved in the resolution). */
    pair.margin = 1.0;
    pair.timestamp = resolution_ts;

    const char *src = HU_INIT_DPO_BRIDGE_SOURCE;
    size_t src_len = strlen(src);
    size_t copy_s = src_len < sizeof(pair.source) - 1 ? src_len : sizeof(pair.source) - 1;
    memcpy(pair.source, src, copy_s);
    pair.source[copy_s] = '\0';
    pair.source_len = copy_s;

    return hu_dpo_record_pair(s_collector, &pair);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Pairing pass — convert single-sided rows into ORPO-trainable pairs.
 * ────────────────────────────────────────────────────────────────────── */

/* Extract the target handle from a bridge-formatted prompt string.
 * Returns the number of bytes copied to out (excluding NUL), or 0 if
 * the prompt doesn't match the expected shape.
 *
 * Expected: "proactive-proposal: target=<handle> ts=<unix>"
 *
 * Pure predicate — testable in isolation per
 * .claude/rules/security-predicate-extraction.md. (The pairing logic
 * itself is hard to test without a real SQLite collector; the parser
 * is the load-bearing piece.) */
static size_t pb_extract_target(const char *prompt, char *out, size_t out_cap) {
    if (!prompt || !out || out_cap == 0)
        return 0;
    const char *anchor = "target=";
    const char *p = strstr(prompt, anchor);
    if (!p)
        return 0;
    p += strlen(anchor);
    /* Read until whitespace or end-of-string. */
    size_t len = 0;
    while (p[len] && p[len] != ' ' && p[len] != '\t' && p[len] != '\n' && len < out_cap - 1)
        len++;
    memcpy(out, p, len);
    out[len] = '\0';
    return len;
}

#ifdef HU_ENABLE_SQLITE

/* In-memory row representation for the pairing walk. Bounded at a
 * conservative 1024 — the daemon's resolver writes at most one row per
 * proposer tick (default 30 min), so 1024 represents ~21 days of
 * resolutions. Past that, the pairing pass processes the head and
 * leaves the tail for the next invocation (idempotent). */
typedef struct pair_row {
    int64_t id;
    int64_t timestamp;
    char target[64];
    char draft[4096];
    bool is_replied; /* true iff chosen non-empty AND rejected empty */
} pair_row_t;

#define HU_INIT_DPO_PAIRING_BATCH 1024

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_init_dpo_bridge_pair_singles(hu_allocator_t *alloc, size_t *paired_count) {
    if (paired_count)
        *paired_count = 0;
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!s_collector)
        return HU_ERR_NOT_SUPPORTED;

#ifndef HU_ENABLE_SQLITE
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!s_collector->db)
        return HU_ERR_NOT_SUPPORTED;

    pair_row_t *rows =
        (pair_row_t *)alloc->alloc(alloc->ctx, sizeof(pair_row_t) * HU_INIT_DPO_PAIRING_BATCH);
    if (!rows)
        return HU_ERR_OUT_OF_MEMORY;
    memset(rows, 0, sizeof(pair_row_t) * HU_INIT_DPO_PAIRING_BATCH);

    /* Walk unprocessed single-sided rows in temporal order. The
     * `margin != ?` filter implements the sentinel-based idempotency
     * mark — already-paired rows are skipped on re-run. */
    sqlite3_stmt *sel = NULL;
    const char *sql_select = "SELECT id, prompt, chosen, rejected, timestamp FROM dpo_pairs "
                             "WHERE source = ? AND margin != ? "
                             "ORDER BY timestamp ASC LIMIT ?";
    int rc = sqlite3_prepare_v2(s_collector->db, sql_select, -1, &sel, NULL);
    if (rc != SQLITE_OK) {
        alloc->free(alloc->ctx, rows, sizeof(pair_row_t) * HU_INIT_DPO_PAIRING_BATCH);
        return HU_ERR_IO;
    }
    sqlite3_bind_text(sel, 1, HU_INIT_DPO_BRIDGE_SOURCE, -1, SQLITE_STATIC);
    sqlite3_bind_double(sel, 2, HU_INIT_DPO_BRIDGE_PAIRED_MARGIN_SENTINEL);
    sqlite3_bind_int(sel, 3, HU_INIT_DPO_PAIRING_BATCH);

    size_t row_count = 0;
    while (sqlite3_step(sel) == SQLITE_ROW && row_count < HU_INIT_DPO_PAIRING_BATCH) {
        pair_row_t *r = &rows[row_count];
        r->id = sqlite3_column_int64(sel, 0);
        const unsigned char *prompt = sqlite3_column_text(sel, 1);
        const unsigned char *chosen = sqlite3_column_text(sel, 2);
        const unsigned char *rejected = sqlite3_column_text(sel, 3);
        r->timestamp = sqlite3_column_int64(sel, 4);

        if (prompt)
            (void)pb_extract_target((const char *)prompt, r->target, sizeof(r->target));
        if (r->target[0] == '\0')
            continue; /* malformed prompt — skip */

        size_t chosen_len = chosen ? strlen((const char *)chosen) : 0;
        size_t rejected_len = rejected ? strlen((const char *)rejected) : 0;
        if (chosen_len >= 4 && rejected_len == 0) {
            r->is_replied = true;
            size_t copy = chosen_len < sizeof(r->draft) - 1 ? chosen_len : sizeof(r->draft) - 1;
            memcpy(r->draft, chosen, copy);
            r->draft[copy] = '\0';
        } else if (rejected_len >= 4 && chosen_len == 0) {
            r->is_replied = false;
            size_t copy = rejected_len < sizeof(r->draft) - 1 ? rejected_len : sizeof(r->draft) - 1;
            memcpy(r->draft, rejected, copy);
            r->draft[copy] = '\0';
        } else {
            continue; /* not a clean single-sided row — skip */
        }
        row_count++;
    }
    sqlite3_finalize(sel);

    /* Pair rows by target: most-recent REPLIED with most-recent IGNORED
     * where IGNORED ts < REPLIED ts. Walk back-to-front (newest first).
     * O(N^2) worst case but bounded by HU_INIT_DPO_PAIRING_BATCH. */
    bool *consumed = (bool *)alloc->alloc(alloc->ctx, sizeof(bool) * row_count);
    if (!consumed && row_count > 0) {
        alloc->free(alloc->ctx, rows, sizeof(pair_row_t) * HU_INIT_DPO_PAIRING_BATCH);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (consumed)
        memset(consumed, 0, sizeof(bool) * row_count);

    size_t paired = 0;
    int64_t now_ts = (int64_t)time(NULL);

    for (size_t i = row_count; i-- > 0;) {
        if (consumed[i] || !rows[i].is_replied)
            continue;
        /* Look back for an IGNORED row with the same target and earlier ts. */
        size_t match = SIZE_MAX;
        for (size_t j = i; j-- > 0;) {
            if (consumed[j] || rows[j].is_replied)
                continue;
            if (rows[j].timestamp >= rows[i].timestamp)
                continue;
            if (strcmp(rows[j].target, rows[i].target) != 0)
                continue;
            match = j;
            break;
        }
        if (match == SIZE_MAX)
            continue;

        /* Build the paired row. */
        hu_preference_pair_t pair;
        memset(&pair, 0, sizeof(pair));
        int pn = snprintf(pair.prompt, sizeof(pair.prompt),
                          "proactive-proposal-paired: target=%s ts=%lld", rows[i].target,
                          (long long)now_ts);
        if (pn < 0) {
            continue;
        }
        pair.prompt_len = (size_t)pn;

        size_t cl = strlen(rows[i].draft);
        size_t copy_c = cl < sizeof(pair.chosen) - 1 ? cl : sizeof(pair.chosen) - 1;
        memcpy(pair.chosen, rows[i].draft, copy_c);
        pair.chosen[copy_c] = '\0';
        pair.chosen_len = copy_c;

        size_t rl = strlen(rows[match].draft);
        size_t copy_r = rl < sizeof(pair.rejected) - 1 ? rl : sizeof(pair.rejected) - 1;
        memcpy(pair.rejected, rows[match].draft, copy_r);
        pair.rejected[copy_r] = '\0';
        pair.rejected_len = copy_r;

        pair.margin = 1.0;
        pair.timestamp = now_ts;

        const char *src = HU_INIT_DPO_BRIDGE_PAIRED_SOURCE;
        size_t src_len = strlen(src);
        size_t copy_s = src_len < sizeof(pair.source) - 1 ? src_len : sizeof(pair.source) - 1;
        memcpy(pair.source, src, copy_s);
        pair.source[copy_s] = '\0';
        pair.source_len = copy_s;

        if (hu_dpo_record_pair(s_collector, &pair) != HU_OK)
            continue;

        /* Mark BOTH source rows with the sentinel margin so a re-run
         * doesn't re-pair them. We do this as a single UPDATE with
         * IN(?, ?). Belt-and-suspenders: also mark consumed[] so this
         * call's later iterations don't try to re-use them. */
        sqlite3_stmt *upd = NULL;
        const char *sql_upd = "UPDATE dpo_pairs SET margin = ? WHERE id IN (?, ?)";
        if (sqlite3_prepare_v2(s_collector->db, sql_upd, -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_double(upd, 1, HU_INIT_DPO_BRIDGE_PAIRED_MARGIN_SENTINEL);
            sqlite3_bind_int64(upd, 2, rows[i].id);
            sqlite3_bind_int64(upd, 3, rows[match].id);
            (void)sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
        consumed[i] = true;
        consumed[match] = true;
        paired++;
    }

    if (consumed)
        alloc->free(alloc->ctx, consumed, sizeof(bool) * row_count);
    alloc->free(alloc->ctx, rows, sizeof(pair_row_t) * HU_INIT_DPO_PAIRING_BATCH);

    if (paired_count)
        *paired_count = paired;
    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
}

/* ──────────────────────────────────────────────────────────────────────────
 * CLI handler — `human ml pair-init-singles`
 *
 * Operator-facing entry point that owns the lifecycle: opens the
 * SQLite db, creates a collector, registers it with the bridge, runs
 * the pairing pass, prints the result, and tears down. Distinct from
 * the daemon-side auto-pair tick — that one reuses the daemon's
 * already-registered collector.
 * ────────────────────────────────────────────────────────────────────── */

hu_error_t hu_ml_cli_pair_init_singles(hu_allocator_t *alloc, int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef HU_ENABLE_SQLITE
    fprintf(stderr, "[pair-init-singles] HU_ENABLE_SQLITE not compiled in; "
                    "the pairing pass requires the dpo_pairs table.\n");
    return HU_ERR_NOT_SUPPORTED;
#else
    /* Resolve memory.db path. Honors HU_MEMORY_DB_PATH env override
     * so tests can point at a controlled fixture without writing into
     * the operator's real db. */
    char db_path[1024];
    const char *override = getenv("HU_MEMORY_DB_PATH");
    if (override && override[0]) {
        snprintf(db_path, sizeof(db_path), "%s", override);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            fprintf(stderr, "[pair-init-singles] HOME not set; cannot locate memory.db\n");
            return HU_ERR_IO;
        }
        snprintf(db_path, sizeof(db_path), "%s/.human/memory.db", home);
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "[pair-init-singles] failed to open %s: %s\n", db_path,
                db ? sqlite3_errmsg(db) : "(null)");
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    hu_dpo_collector_t col;
    hu_error_t cerr = hu_dpo_collector_create(alloc, db, 0, &col);
    if (cerr != HU_OK) {
        fprintf(stderr, "[pair-init-singles] collector_create failed: error %d\n", (int)cerr);
        sqlite3_close(db);
        return cerr;
    }
    /* Ensure tables exist — idempotent. A fresh memory.db without
     * dpo_pairs would otherwise yield "no such table" on the SELECT. */
    (void)hu_dpo_init_tables(&col);

    /* Save the prior bridge collector (might be NULL in CLI context,
     * but could be set if someone runs the CLI inside a process where
     * the daemon has already registered). Restore at exit so we don't
     * leave dangling state. */
    struct hu_dpo_collector *prior = hu_init_dpo_bridge_get_collector();
    hu_init_dpo_bridge_set_collector(&col);

    size_t paired = 0;
    hu_error_t perr = hu_init_dpo_bridge_pair_singles(alloc, &paired);

    /* Restore prior collector BEFORE deiniting our local one — if the
     * order were reversed, any code running between the deinit and the
     * restore would see the singleton pointing at a freed handle. */
    hu_init_dpo_bridge_set_collector(prior);
    hu_dpo_collector_deinit(&col);
    sqlite3_close(db);

    if (perr != HU_OK) {
        fprintf(stderr, "[pair-init-singles] pair_singles failed: error %d\n", (int)perr);
        return perr;
    }
    fprintf(stdout, "[pair-init-singles] paired %zu singles from %s\n", paired, db_path);
    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
}

#endif /* HU_ENABLE_ML */
