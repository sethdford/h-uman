/* confidence_repo_sqlite.c — provenance columns on `memories` for the
 * confidence boundary. Contract: include/human/memory/confidence_repo.h. */
#include "human/memory/confidence_repo.h"

#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

#define CB_BACKFILL_BATCH 500

static void add_column(sqlite3 *db, const char *sql) {
    char *err = NULL;
    sqlite3_exec(db, sql, NULL, NULL, &err); /* "duplicate column" on re-run */
    if (err)
        sqlite3_free(err);
}

static void update_row(sqlite3 *db, const char *where_sql, int64_t rowid, const char *key,
                       size_t key_len, hu_share_level_t level, const char *contact) {
    char sql[160];
    snprintf(sql, sizeof(sql), "UPDATE memories SET share_level = ?1, source_contact = ?2 %s",
             where_sql);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, (int)level);
    sqlite3_bind_text(st, 2, contact, -1, SQLITE_STATIC);
    if (key)
        sqlite3_bind_text(st, 3, key, (int)key_len, SQLITE_STATIC);
    else
        sqlite3_bind_int64(st, 3, rowid);
    (void)sqlite3_step(st);
    sqlite3_finalize(st);
}

int hu_confidence_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return -1;
    add_column(db, "ALTER TABLE memories ADD COLUMN source_contact TEXT");
    add_column(db, "ALTER TABLE memories ADD COLUMN share_level INTEGER");
    int total = 0;
    for (;;) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT rowid, key, COALESCE(session_id, ''), COALESCE(source, '') "
                               "FROM memories WHERE share_level IS NULL LIMIT ?1",
                               -1, &st, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_int(st, 1, CB_BACKFILL_BATCH);
        int64_t rowids[CB_BACKFILL_BATCH];
        hu_share_level_t levels[CB_BACKFILL_BATCH];
        char contacts[CB_BACKFILL_BATCH][HU_CB_CONTACT_MAX];
        int n = 0;
        while (n < CB_BACKFILL_BATCH && sqlite3_step(st) == SQLITE_ROW) {
            const char *key = (const char *)sqlite3_column_text(st, 1);
            const char *sess = (const char *)sqlite3_column_text(st, 2);
            const char *src = (const char *)sqlite3_column_text(st, 3);
            rowids[n] = sqlite3_column_int64(st, 0);
            levels[n] = hu_confidence_derive_row(
                key, key ? strlen(key) : 0, sess, sess ? strlen(sess) : 0, src,
                src ? strlen(src) : 0, NULL, 0, contacts[n], sizeof(contacts[n]));
            n++;
        }
        sqlite3_finalize(st);
        if (n == 0)
            break;
        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        for (int i = 0; i < n; i++)
            update_row(db, "WHERE rowid = ?3", rowids[i], NULL, 0, levels[i], contacts[i]);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        total += n;
        if (n < CB_BACKFILL_BATCH)
            break;
    }
    return total;
}

/* Read key's session and source; false when the key is not stored. */
static bool row_origin(sqlite3 *db, const char *key, size_t key_len, char *sess, size_t sess_cap,
                       char *src, size_t src_cap) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COALESCE(session_id, ''), COALESCE(source, '') FROM memories "
                           "WHERE key = ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        snprintf(sess, sess_cap, "%s", (const char *)sqlite3_column_text(st, 0));
        snprintf(src, src_cap, "%s", (const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return found;
}

static void stamp(sqlite3 *db, const char *key, size_t key_len, const char *write_contact,
                  size_t write_contact_len, bool owner_when_unknown) {
    char sess[HU_CB_CONTACT_MAX], src[64], contact[HU_CB_CONTACT_MAX];
    if (!db || !key || key_len == 0 ||
        !row_origin(db, key, key_len, sess, sizeof(sess), src, sizeof(src)))
        return;
    hu_share_level_t level =
        hu_confidence_derive_row(key, key_len, sess, strlen(sess), src, strlen(src), write_contact,
                                 write_contact_len, contact, sizeof(contact));
    if (owner_when_unknown && level == HU_SHARE_PRIVATE_TO_SOURCE && contact[0] == '\0')
        level = HU_SHARE_OWNER_SELF;
    update_row(db, "WHERE key = ?3", 0, key, key_len, level, contact);
}

void hu_confidence_repo_stamp(sqlite3 *db, const char *key, size_t key_len, const char *session,
                              size_t session_len) {
    (void)session; /* re-read with the row's source: the upsert may have kept it */
    (void)session_len;
    stamp(db, key, key_len, NULL, 0, false);
}

void hu_confidence_repo_stamp_write(hu_memory_t *mem, const char *key, size_t key_len,
                                    const char *contact, size_t contact_len) {
    bool owner = !contact || contact_len == 0;
    stamp(hu_sqlite_memory_get_db(mem), key, key_len, contact, owner ? 0 : contact_len, owner);
}

bool hu_confidence_repo_lookup(hu_memory_t *mem, const char *key, size_t key_len,
                               hu_share_level_t *level, char *contact, size_t cap) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    if (contact && cap)
        contact[0] = '\0';
    if (!db || !key || key_len == 0 || !level)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COALESCE(share_level, 0), COALESCE(source_contact, '') "
                           "FROM memories WHERE key = ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, key, (int)key_len, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        int v = sqlite3_column_int(st, 0);
        *level = (v >= HU_SHARE_UNSET && v <= HU_SHARE_OWNER_SELF) ? (hu_share_level_t)v
                                                                   : HU_SHARE_UNSET;
        if (contact && cap)
            snprintf(contact, cap, "%s", (const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return found;
}

#else /* !HU_ENABLE_SQLITE */

bool hu_confidence_repo_lookup(hu_memory_t *mem, const char *key, size_t key_len,
                               hu_share_level_t *level, char *contact, size_t cap) {
    (void)mem;
    (void)key;
    (void)key_len;
    (void)level;
    if (contact && cap)
        contact[0] = '\0';
    return false;
}

void hu_confidence_repo_stamp_write(hu_memory_t *mem, const char *key, size_t key_len,
                                    const char *contact, size_t contact_len) {
    (void)mem;
    (void)key;
    (void)key_len;
    (void)contact;
    (void)contact_len;
}

#endif
