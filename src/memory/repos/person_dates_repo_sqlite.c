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
                                ");");
}

hu_error_t hu_person_dates_repo_set(sqlite3 *db, const char *contact_id, const char *label,
                                    int month, int day, const char *source, int64_t now) {
    if (!db || !contact_id || !contact_id[0] || !label || !label[0] || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *stmt = NULL;
    hu_error_t err = hu_person_dates_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO person_dates (contact_id, label, month, day, source, "
                           "updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
                           "ON CONFLICT (contact_id, label) DO UPDATE SET month=excluded.month, "
                           "day=excluded.day, source=excluded.source, "
                           "updated_at=excluded.updated_at;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;
    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, month);
    sqlite3_bind_int(stmt, 4, day);
    sqlite3_bind_text(stmt, 5, source ? source : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, now);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_STORE;
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
