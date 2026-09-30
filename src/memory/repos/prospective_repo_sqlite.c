/*
 * src/memory/repos/prospective_repo_sqlite.c
 *
 * SQLite-backed typed intention store (prospective memory v2). Contract in
 * include/human/memory/prospective_repo.h; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1.
 */
#include "human/memory/prospective_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool pm_has_column(sqlite3 *db, const char *col) {
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(prospective_memories)", -1, &st, NULL) !=
        SQLITE_OK)
        return false;
    while (!found && sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 1);
        found = name && strcmp(name, col) == 0;
    }
    sqlite3_finalize(st);
    return found;
}

static const struct {
    const char *name;
    const char *ddl;
} k_pm_columns[] = {
    {"cue_kind",
     "ALTER TABLE prospective_memories ADD COLUMN cue_kind TEXT NOT NULL DEFAULT 'keyword'"},
    {"due_at", "ALTER TABLE prospective_memories ADD COLUMN due_at INTEGER"},
    {"status",
     "ALTER TABLE prospective_memories ADD COLUMN status TEXT NOT NULL DEFAULT 'pending'"},
    {"surfaced_at", "ALTER TABLE prospective_memories ADD COLUMN surfaced_at INTEGER"},
    {"attempts", "ALTER TABLE prospective_memories ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0"},
    {"outcome", "ALTER TABLE prospective_memories ADD COLUMN outcome TEXT"},
    {"source",
     "ALTER TABLE prospective_memories ADD COLUMN source TEXT NOT NULL DEFAULT 'extractor'"},
};

/* fired -> status for rows still at the default, the status index, and the
 * trigger that keeps status in step with any later legacy write of fired.
 * All idempotent, so it runs on every open (cheap: one indexed UPDATE). */
static const char k_pm_derived[] =
    "UPDATE prospective_memories SET status = CASE fired WHEN 1 THEN 'done' "
    "WHEN 2 THEN 'canceled' WHEN 3 THEN 'expired' ELSE status END "
    "WHERE status = 'pending' AND fired IN (1, 2, 3);"
    "CREATE INDEX IF NOT EXISTS idx_prospective_status ON "
    "prospective_memories(contact_id, cue_kind, status);"
    "CREATE TRIGGER IF NOT EXISTS trg_prospective_fired_status "
    "AFTER UPDATE OF fired ON prospective_memories WHEN NEW.fired IS NOT OLD.fired "
    "BEGIN UPDATE prospective_memories SET status = CASE NEW.fired WHEN 1 THEN 'done' "
    "WHEN 2 THEN 'canceled' WHEN 3 THEN 'expired' ELSE 'pending' END WHERE id = NEW.id; END;";

hu_error_t hu_prospective_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (!pm_has_column(db, "action"))
        return HU_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(k_pm_columns) / sizeof(k_pm_columns[0]); i++) {
        if (pm_has_column(db, k_pm_columns[i].name))
            continue;
        hu_error_t e = hu_repo_exec_ddl(db, k_pm_columns[i].ddl);
        if (e != HU_OK)
            return e;
    }
    return hu_repo_exec_ddl(db, k_pm_derived);
}

static void pm_col_copy(sqlite3_stmt *st, int col, char *dst, size_t cap) {
    const unsigned char *s = sqlite3_column_text(st, col);
    size_t n = s ? (size_t)sqlite3_column_bytes(st, col) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, s, n);
    dst[n] = '\0';
}

static void pm_decode(sqlite3_stmt *st, hu_prospective_item_t *it) {
    memset(it, 0, sizeof(*it));
    it->id = sqlite3_column_int64(st, 0);
    if (!hu_prospective_cue_kind_parse((const char *)sqlite3_column_text(st, 1), &it->cue_kind))
        it->cue_kind = HU_PM_CUE_KEYWORD;
    if (!hu_prospective_status_parse((const char *)sqlite3_column_text(st, 2), &it->status))
        it->status = HU_PM_EXPIRED; /* unknown state: never eligible */
    pm_col_copy(st, 3, it->trigger_value, sizeof(it->trigger_value));
    pm_col_copy(st, 4, it->action, sizeof(it->action));
    pm_col_copy(st, 5, it->contact_id, sizeof(it->contact_id));
    it->due_at = sqlite3_column_int64(st, 6);
    it->expires_at = sqlite3_column_int64(st, 7);
    it->created_at = sqlite3_column_int64(st, 8);
    it->surfaced_at = sqlite3_column_int64(st, 9);
    it->attempts = sqlite3_column_int(st, 10);
}

static void pm_bind_contact(sqlite3_stmt *st, int idx, const char *contact, size_t len) {
    if (contact && len > 0)
        sqlite3_bind_text(st, idx, contact, (int)len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, idx);
}

static const char k_pm_list[] =
    "SELECT id, cue_kind, status, trigger_value, action, contact_id, due_at, expires_at, "
    "created_at, surfaced_at, attempts FROM prospective_memories "
    "WHERE cue_kind = ?1 AND status = ?2 "
    "AND (contact_id = ?3 OR (?1 = 'keyword' AND contact_id IS NULL)) "
    "AND (?1 <> 'keyword' OR trigger_type = 'keyword') "
    "ORDER BY CASE WHEN ?1 = 'time' THEN due_at END ASC, created_at DESC, id DESC";

hu_error_t hu_prospective_repo_list(hu_allocator_t *alloc, sqlite3 *db,
                                    hu_prospective_cue_kind_t kind, hu_prospective_status_t status,
                                    const char *contact, size_t contact_len,
                                    hu_prospective_item_t **out, size_t *out_count) {
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    const char *kind_s = hu_prospective_cue_kind_str(kind);
    const char *status_s = hu_prospective_status_str(status);
    if (!alloc || !db || !out || !out_count || !kind_s || !status_s)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_list, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, kind_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, status_s, -1, SQLITE_STATIC);
    pm_bind_contact(st, 3, contact, contact_len);
    size_t cap = 0;
    size_t n = 0;
    hu_prospective_item_t *arr = NULL;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            hu_prospective_item_t *nb = (hu_prospective_item_t *)alloc->alloc(
                alloc->ctx, nc * sizeof(hu_prospective_item_t));
            if (!nb) {
                if (arr)
                    alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
                sqlite3_finalize(st);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (arr) {
                memcpy(nb, arr, n * sizeof(hu_prospective_item_t));
                alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
            }
            arr = nb;
            cap = nc;
        }
        pm_decode(st, &arr[n++]);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || n == 0) {
        if (arr)
            alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
        return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
    }
    /* Hand back exactly n items so the caller frees with the count it holds. */
    hu_prospective_item_t *exact =
        (hu_prospective_item_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_prospective_item_t));
    if (exact)
        memcpy(exact, arr, n * sizeof(hu_prospective_item_t));
    alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
    if (!exact)
        return HU_ERR_OUT_OF_MEMORY;
    *out = exact;
    *out_count = n;
    return HU_OK;
}

void hu_prospective_repo_free(hu_allocator_t *alloc, hu_prospective_item_t *items, size_t count) {
    if (alloc && items)
        alloc->free(alloc->ctx, items, count * sizeof(hu_prospective_item_t));
}

/* The `status IN ('pending', 'surfaced')` guard is a read-then-write on the
 * open-row set, not a compare-and-set against a version the caller observed —
 * it assumes a single writer per contact (the daemon loop). A second
 * concurrent transition on the same contact could still move an intention
 * the caller thought was untouched.
 *
 * `hu_prospective_status_to_fired` maps both PENDING and SURFACED to
 * fired=0 (only DONE/CANCELED/EXPIRED get 1/2/3). That is what keeps this
 * UPDATE from fighting `trg_prospective_fired_status`: the trigger fires
 * AFTER UPDATE OF fired and rewrites status from NEW.fired, but since a
 * SURFACED write leaves fired at 0 the trigger's own fired=0 branch maps
 * back to 'pending' — a no-op against the status this statement just set,
 * because fired did not actually change (0 -> 0 trips WHEN NEW.fired IS NOT
 * OLD.fired only when it truly changes). Changing either mapping (e.g.
 * giving SURFACED its own fired code) reintroduces the fight: the trigger
 * would then overwrite this statement's 'surfaced' back to whatever its
 * CASE maps that new fired value to. */
static const char k_pm_transition[] =
    "UPDATE prospective_memories SET status = ?1, fired = ?2, attempts = ?3, "
    "outcome = COALESCE(?4, outcome), "
    "surfaced_at = CASE WHEN ?1 = 'surfaced' THEN ?5 ELSE surfaced_at END "
    "WHERE action = ?6 AND cue_kind = ?7 AND status IN ('pending', 'surfaced') "
    "AND ((?8 IS NULL AND contact_id IS NULL) OR contact_id = ?8)";

hu_error_t hu_prospective_repo_transition(sqlite3 *db, const hu_prospective_item_t *it,
                                          hu_prospective_status_t to,
                                          hu_prospective_outcome_t outcome, int attempts,
                                          int64_t now, int *changed) {
    if (changed)
        *changed = 0;
    const char *to_s = hu_prospective_status_str(to);
    const char *kind_s = it ? hu_prospective_cue_kind_str(it->cue_kind) : NULL;
    if (!db || !it || !to_s || !kind_s || !it->action[0])
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_transition, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    const char *out_s = hu_prospective_outcome_str(outcome);
    sqlite3_bind_text(st, 1, to_s, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, hu_prospective_status_to_fired(to));
    sqlite3_bind_int(st, 3, attempts);
    if (out_s)
        sqlite3_bind_text(st, 4, out_s, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 4);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_text(st, 6, it->action, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 7, kind_s, -1, SQLITE_STATIC);
    pm_bind_contact(st, 8, it->contact_id, strlen(it->contact_id));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (changed)
        *changed = sqlite3_changes(db);
    return HU_OK;
}

/* Matching is by contact + text (COUNT(DISTINCT action)): by design,
 * identically-worded open items for one contact are treated as one promise,
 * so a keyword row and its time-cue twin (or two writers producing the same
 * wording) count once, not twice. */
hu_error_t hu_prospective_repo_count_surfaced_since(sqlite3 *db, hu_prospective_cue_kind_t kind,
                                                    const char *contact, size_t contact_len,
                                                    int64_t since, int64_t *out) {
    if (out)
        *out = 0;
    const char *kind_s = hu_prospective_cue_kind_str(kind);
    if (!db || !out || !kind_s || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(DISTINCT action) FROM prospective_memories WHERE "
                           "cue_kind = ?1 AND contact_id = ?2 AND surfaced_at >= ?3",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, kind_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, since);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

/* Trim + case-fold + collapse-internal-whitespace, for the F1 open-row
 * dedupe: two writers describing the same still-open promise in slightly
 * different casing/spacing must compare equal. `out` is NUL-terminated and
 * truncates rather than overflows `cap`. */
static void pm_normalize_action(const char *action, size_t len, char *out, size_t cap) {
    if (cap == 0)
        return;
    size_t i = 0, j = len;
    while (i < j && isspace((unsigned char)action[i]))
        i++;
    while (j > i && isspace((unsigned char)action[j - 1]))
        j--;
    size_t oi = 0;
    bool in_ws = false;
    for (; i < j && oi + 1 < cap; i++) {
        unsigned char c = (unsigned char)action[i];
        if (isspace(c)) {
            if (!in_ws) {
                out[oi++] = ' ';
                in_ws = true;
            }
        } else {
            out[oi++] = (char)tolower(c);
            in_ws = false;
        }
    }
    out[oi] = '\0';
}

/* Exact source-key match, any status: the same backfill/source event must
 * never be recorded twice, even if the earlier row already settled. */
static const char k_pm_key_exists[] =
    "SELECT 1 FROM prospective_memories WHERE cue_kind = 'time' AND trigger_value = ?1 LIMIT 1";

/* Candidate OPEN rows (pending/surfaced only — a settled row never blocks a
 * fresh intention) for this contact. Fix round 1 (I1): earlier this repo
 * asked SQLite to normalize the stored `action` inline
 * (LOWER/TRIM + two REPLACE('  ',' ') passes) and compare it to a
 * C-normalized parameter. That SQL-side approximation only collapses a
 * whitespace run by roughly half per REPLACE pass (SQLite's REPLACE does
 * one left-to-right, non-overlapping scan), so a run of 5+ characters left
 * a residual double space and the two sides never matched — NOT EXISTS was
 * always true and a duplicate open intention got inserted, exactly what F1
 * exists to prevent. There is no number of REPLACE passes that is provably
 * enough for an unbounded run. The fix: never normalize in SQL. Fetch every
 * OPEN row's *raw* action for this contact and run the SAME C function
 * (`pm_normalize_action`) on each candidate that already normalizes the
 * input, then compare in C. One normalizer, two call sites, no drift. */
static const char k_pm_open_candidates[] =
    "SELECT action FROM prospective_memories WHERE cue_kind = 'time' AND contact_id = ?1 "
    "AND status IN ('pending', 'surfaced')";

static const char k_pm_insert_time[] =
    "INSERT INTO prospective_memories(trigger_type, trigger_value, action, contact_id, "
    "expires_at, fired, created_at, cue_kind, due_at, status, source) "
    "VALUES('time', ?1, ?2, ?3, ?4, ?5, ?6, 'time', ?7, ?8, ?9)";

/* F1: "already present" for a cue_kind='time' upsert means either the exact
 * source key (k_pm_key_exists, any status) or — for a row still OPEN
 * (pending/surfaced) — the same contact + the same action once both are
 * trimmed, case-folded and internal-whitespace-collapsed, IGNORING due_at:
 * a backfill re-run that re-anchors due_at (a fresh source key, a later
 * due_at) must not create a second intention for the same still-open
 * promise. See k_pm_open_candidates above for why that comparison is done
 * in C, never in SQL. */
hu_error_t hu_prospective_repo_upsert_time(sqlite3 *db, const char *contact, size_t contact_len,
                                           const char *action, size_t action_len, int64_t due_at,
                                           int64_t grace_s, hu_prospective_source_t source,
                                           const char *source_key, hu_prospective_status_t status,
                                           int64_t now, bool *inserted) {
    if (inserted)
        *inserted = false;
    const char *status_s = hu_prospective_status_str(status);
    const char *source_s = hu_prospective_source_str(source);
    if (!db || !contact || contact_len == 0 || !action || action_len == 0 || !source_key ||
        !source_key[0] || due_at <= 0 || !status_s || !source_s)
        return HU_ERR_INVALID_ARGUMENT;
    char norm[512];
    pm_normalize_action(action, action_len, norm, sizeof(norm));

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_key_exists, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, source_key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    bool dedupe_hit = rc == SQLITE_ROW;
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;

    if (!dedupe_hit) {
        st = NULL;
        if (sqlite3_prepare_v2(db, k_pm_open_candidates, -1, &st, NULL) != SQLITE_OK)
            return HU_ERR_MEMORY_BACKEND;
        sqlite3_bind_text(st, 1, contact, (int)contact_len, SQLITE_STATIC);
        char cand_norm[512];
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const unsigned char *cand = sqlite3_column_text(st, 0);
            size_t cand_len = cand ? (size_t)sqlite3_column_bytes(st, 0) : 0;
            pm_normalize_action((const char *)cand, cand_len, cand_norm, sizeof(cand_norm));
            if (strcmp(cand_norm, norm) == 0) {
                dedupe_hit = true;
                break;
            }
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            return HU_ERR_MEMORY_BACKEND;
    }

    if (dedupe_hit)
        return HU_OK; /* *inserted already false */

    st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_insert_time, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, source_key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, action, (int)action_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, due_at + grace_s);
    sqlite3_bind_int(st, 5, hu_prospective_status_to_fired(status));
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_int64(st, 7, due_at);
    sqlite3_bind_text(st, 8, status_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, source_s, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (inserted)
        *inserted = true;
    return HU_OK;
}

/* Matching is by contact + text (contact_id + description / contact_id +
 * topic, exact): by design, identically-worded open items for one contact
 * are treated as one promise, so this retires every ledger row that reads
 * as the same commitment for that contact, not a specific row id. Each
 * WHERE also requires the ledger's own "still open" state (status='pending'
 * / sent=0), so a second call for an already-settled row matches nothing
 * and is a no-op. */
hu_error_t hu_prospective_repo_sync_source(sqlite3 *db, const hu_prospective_item_t *it,
                                           hu_prospective_status_t to, int64_t now) {
    const char *ledger = to == HU_PM_DONE       ? "followed_up"
                         : to == HU_PM_CANCELED ? "canceled"
                         : to == HU_PM_EXPIRED  ? "expired"
                                                : NULL;
    if (!db || !it || !it->contact_id[0] || !it->action[0] || !ledger)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE commitments SET status = ?1, followed_up_at = ?2 WHERE "
                           "contact_id = ?3 AND description = ?4 AND status = 'pending'",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, ledger, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    sqlite3_bind_text(st, 3, it->contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, it->action, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (sqlite3_prepare_v2(db,
                           "UPDATE delayed_followups SET sent = 1 WHERE contact_id = ?1 AND "
                           "topic = ?2 AND sent = 0",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, it->contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, it->action, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

#endif /* HU_ENABLE_SQLITE */
