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

#endif /* HU_ENABLE_SQLITE */
