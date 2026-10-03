/*
 * src/memory/repos/person_dates_repo_sqlite.c
 *
 * Person dates and Contacts birthdays. Contract: include/human/memory/person_dates_repo.h.
 */
#include "human/memory/person_dates_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Core Data stores dates as seconds since 2001-01-01T00:00:00Z. */
#define CORE_DATA_EPOCH_OFFSET 978307200LL

hu_error_t hu_person_dates_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_repo_exec_ddl(db, "CREATE TABLE IF NOT EXISTS person_dates ("
                                "  contact_id TEXT NOT NULL,"
                                "  label TEXT NOT NULL,"
                                "  month INTEGER NOT NULL CHECK (month BETWEEN 1 AND 12),"
                                "  day INTEGER NOT NULL CHECK (day BETWEEN 1 AND 31),"
                                "  source TEXT,"
                                "  updated_at INTEGER NOT NULL,"
                                "  UNIQUE (contact_id, label)"
                                ");"
                                "CREATE TABLE IF NOT EXISTS date_drafts ("
                                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "  day TEXT NOT NULL,"
                                "  contact_id TEXT NOT NULL,"
                                "  label TEXT NOT NULL,"
                                "  draft TEXT NOT NULL,"
                                "  final_text TEXT,"
                                "  status TEXT NOT NULL DEFAULT 'asked' CHECK (status IN "
                                "    ('asked','approved','sent','skipped','failed')),"
                                "  created_at INTEGER NOT NULL,"
                                "  decided_at INTEGER,"
                                "  UNIQUE (day, contact_id, label)"
                                ");");
}

/* Ensure the schema, then prepare `sql`. */
static hu_error_t prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) {
    hu_error_t err = hu_person_dates_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    return sqlite3_prepare_v2(db, sql, -1, stmt, NULL) == SQLITE_OK ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_person_dates_repo_set(sqlite3 *db, const char *contact_id, const char *label,
                                    int month, int day, const char *source, int64_t now) {
    if (!db || !contact_id || !contact_id[0] || !label || !label[0] || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    hu_error_t err = prepare(db,
                             "INSERT INTO person_dates (contact_id, label, month, day, source, "
                             "updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
                             "ON CONFLICT (contact_id, label) DO UPDATE SET "
                             "month=excluded.month, day=excluded.day, source=excluded.source, "
                             "updated_at=excluded.updated_at;",
                             &stmt);
    if (err != HU_OK)
        return err;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, month);
    sqlite3_bind_int(stmt, 4, day);
    sqlite3_bind_text(stmt, 5, source ? source : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, now);
    return hu_repo_step_insert(stmt, NULL); /* the upsert always completes */
}

hu_error_t hu_person_dates_repo_list(sqlite3 *db, hu_person_date_t *out, size_t cap,
                                     size_t *out_n) {
    if (!db || !out || cap == 0 || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    hu_error_t err = hu_person_dates_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT contact_id, label, month, day FROM person_dates "
                           "ORDER BY month, day, contact_id LIMIT ?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_int64(stmt, 1, (int64_t)cap);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_n < cap) {
        hu_person_date_t *d = &out[(*out_n)++];
        const unsigned char *c = sqlite3_column_text(stmt, 0);
        const unsigned char *l = sqlite3_column_text(stmt, 1);
        snprintf(d->contact_id, sizeof(d->contact_id), "%s", c ? (const char *)c : "");
        snprintf(d->label, sizeof(d->label), "%s", l ? (const char *)l : "");
        d->month = sqlite3_column_int(stmt, 2);
        d->day = sqlite3_column_int(stmt, 3);
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}

static void read_draft(sqlite3_stmt *st, hu_date_draft_t *d) {
    const unsigned char *t;
    d->id = sqlite3_column_int64(st, 0);
    t = sqlite3_column_text(st, 1);
    snprintf(d->day, sizeof(d->day), "%s", t ? (const char *)t : "");
    t = sqlite3_column_text(st, 2);
    snprintf(d->contact_id, sizeof(d->contact_id), "%s", t ? (const char *)t : "");
    t = sqlite3_column_text(st, 3);
    snprintf(d->label, sizeof(d->label), "%s", t ? (const char *)t : "");
    t = sqlite3_column_text(st, 4);
    snprintf(d->draft, sizeof(d->draft), "%s", t ? (const char *)t : "");
    t = sqlite3_column_text(st, 5);
    snprintf(d->final_text, sizeof(d->final_text), "%s", t ? (const char *)t : "");
}

#define DRAFT_COLUMNS "id, day, contact_id, label, draft, final_text"

hu_error_t hu_date_drafts_repo_ask(sqlite3 *db, const char *day, const char *contact_id,
                                   const char *label, const char *draft, int64_t now,
                                   bool *created) {
    if (!db || !day || !day[0] || !contact_id || !contact_id[0] || !label || !draft || !draft[0] ||
        !created)
        return HU_ERR_INVALID_ARGUMENT;
    *created = false;
    sqlite3_stmt *stmt = NULL;
    hu_error_t err = prepare(db,
                             "INSERT INTO date_drafts (day, contact_id, label, draft, created_at) "
                             "VALUES (?1, ?2, ?3, ?4, ?5);",
                             &stmt);
    if (err != HU_OK)
        return err;
    sqlite3_bind_text(stmt, 1, day, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, draft, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now);
    return hu_repo_step_insert(stmt, created);
}

/* Rows of `sql` (optionally binding ?1 = text arg), read as drafts. */
static hu_error_t list_drafts(sqlite3 *db, const char *sql, const char *arg, hu_date_draft_t *out,
                              size_t cap, size_t *out_n) {
    *out_n = 0;
    hu_error_t err = hu_person_dates_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    if (arg)
        sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_n < cap)
        read_draft(stmt, &out[(*out_n)++]);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_STORE;
}

hu_error_t hu_date_drafts_repo_open(sqlite3 *db, const char *day, hu_date_draft_t *out) {
    if (!db || !day || !out)
        return HU_ERR_INVALID_ARGUMENT;
    size_t n = 0;
    hu_error_t err = list_drafts(db,
                                 "SELECT " DRAFT_COLUMNS " FROM date_drafts WHERE day=?1 AND "
                                 "status='asked' ORDER BY created_at DESC, id DESC LIMIT 1;",
                                 day, out, 1, &n);
    if (err != HU_OK)
        return err;
    return n == 1 ? HU_OK : HU_ERR_NOT_FOUND;
}

hu_error_t hu_date_drafts_repo_approved(sqlite3 *db, hu_date_draft_t *out, size_t cap,
                                        size_t *out_n) {
    if (!db || !out || cap == 0 || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    return list_drafts(db,
                       "SELECT " DRAFT_COLUMNS " FROM date_drafts WHERE status='approved' "
                       "ORDER BY decided_at, id;",
                       NULL, out, cap, out_n);
}

hu_error_t hu_date_drafts_repo_seen(sqlite3 *db, const char *day, const char *contact_id,
                                    const char *label, bool *seen) {
    if (!db || !day || !contact_id || !label || !seen)
        return HU_ERR_INVALID_ARGUMENT;
    *seen = false;
    sqlite3_stmt *stmt = NULL;
    hu_error_t err = prepare(
        db, "SELECT 1 FROM date_drafts WHERE day=?1 AND contact_id=?2 AND label=?3;", &stmt);
    if (err != HU_OK)
        return err;
    sqlite3_bind_text(stmt, 1, day, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, label, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    *seen = rc == SQLITE_ROW;
    return HU_OK;
}

hu_error_t hu_date_drafts_repo_decide(sqlite3 *db, int64_t id, const char *status,
                                      const char *final_text, int64_t now) {
    if (!db || id <= 0 || !status)
        return HU_ERR_INVALID_ARGUMENT;
    bool approved = strcmp(status, "approved") == 0;
    if (!approved && strcmp(status, "skipped") != 0 && strcmp(status, "sent") != 0 &&
        strcmp(status, "failed") != 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (approved && (!final_text || !final_text[0]))
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE date_drafts SET status=?1, decided_at=?2, "
                           "final_text=COALESCE(?3, final_text) WHERE id=?4;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    if (final_text)
        sqlite3_bind_text(stmt, 3, final_text, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int64(stmt, 4, id);
    return hu_repo_step_update_one(db, stmt);
}

void hu_person_dates_phone_key(const char *s, char *out, size_t cap) {
    char all[64];
    size_t n = 0;
    for (; s && *s && n + 1 < sizeof(all); s++)
        if (isdigit((unsigned char)*s))
            all[n++] = *s;
    all[n] = '\0';
    snprintf(out, cap, "%s", n > 10 ? all + n - 10 : all);
}

/* Birthdays from one database; false when it exists but cannot be read. */
static bool read_one(const char *path, hu_addressbook_birthday_t *out, size_t cap, size_t *n) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(db,
                                 "SELECT p.ZFULLNUMBER, r.ZBIRTHDAY FROM ZABCDPHONENUMBER p "
                                 "JOIN ZABCDRECORD r ON r.Z_PK = p.ZOWNER "
                                 "WHERE r.ZBIRTHDAY IS NOT NULL;",
                                 -1, &stmt, NULL) == SQLITE_OK;
    int rc = SQLITE_DONE;
    while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW && *n < cap) {
        const unsigned char *num = sqlite3_column_text(stmt, 0);
        /* Contacts stores a birthday at noon GMT, so the UTC date is the day. */
        time_t t = (time_t)(sqlite3_column_int64(stmt, 1) + CORE_DATA_EPOCH_OFFSET);
        struct tm tm;
        if (!num || !gmtime_r(&t, &tm))
            continue;
        hu_addressbook_birthday_t *b = &out[*n];
        hu_person_dates_phone_key((const char *)num, b->digits, sizeof(b->digits));
        if (strlen(b->digits) < 7)
            continue;
        b->month = tm.tm_mon + 1;
        b->day = tm.tm_mday;
        (*n)++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok && (rc == SQLITE_DONE || rc == SQLITE_ROW);
}

static void read_if_present(const char *path, hu_addressbook_birthday_t *out, size_t cap, size_t *n,
                            size_t *unreadable) {
    struct stat st;
    if (stat(path, &st) != 0)
        return; /* no database here is not a failure */
    if (!read_one(path, out, cap, n))
        (*unreadable)++;
}

hu_error_t hu_addressbook_birthdays(const char *addressbook_dir, hu_addressbook_birthday_t *out,
                                    size_t cap, size_t *out_n, size_t *out_unreadable) {
    if (!addressbook_dir || !out || cap == 0 || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    size_t unreadable = 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/AddressBook-v22.abcddb", addressbook_dir);
    read_if_present(path, out, cap, out_n, &unreadable);
    char sources[1024];
    snprintf(sources, sizeof(sources), "%s/Sources", addressbook_dir);
    DIR *dir = opendir(sources);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && *out_n < cap) {
            if (e->d_name[0] == '.')
                continue;
            int w =
                snprintf(path, sizeof(path), "%s/%s/AddressBook-v22.abcddb", sources, e->d_name);
            if (w > 0 && (size_t)w < sizeof(path))
                read_if_present(path, out, cap, out_n, &unreadable);
        }
        closedir(dir);
    }
    if (out_unreadable)
        *out_unreadable = unreadable;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
