#ifndef HU_MEMORY_PERSON_DATES_REPO_H
#define HU_MEMORY_PERSON_DATES_REPO_H

/* Dates that belong to a person (life-admin slice 4): birthdays and
 * anniversaries the owner told the daemon about, keyed by contact handle, and
 * birthdays read from the macOS Contacts database. */

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

typedef struct hu_person_date {
    char contact_id[128]; /* persona contact handle, e.g. "+15550000001" */
    char label[32];       /* "birthday", "anniversary" */
    int month, day;       /* 1..12, 1..31 */
} hu_person_date_t;

/* A birthday from Contacts: the last 10 digits of one of the card's phone
 * numbers, and the day. */
typedef struct hu_addressbook_birthday {
    char digits[16];
    int month, day;
} hu_addressbook_birthday_t;

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

hu_error_t hu_person_dates_repo_ensure_schema(sqlite3 *db);

/* Insert or replace the date for (contact_id, label). */
hu_error_t hu_person_dates_repo_set(sqlite3 *db, const char *contact_id, const char *label,
                                    int month, int day, const char *source, int64_t now);

/* All stored dates. */
hu_error_t hu_person_dates_repo_list(sqlite3 *db, hu_person_date_t *out, size_t cap, size_t *out_n);

/* The key a phone number is matched on: its last 10 digits (all of them if
 * fewer), so "+1 (555) 000-0001" and "5550000001" meet. */
void hu_person_dates_phone_key(const char *s, char *out, size_t cap);

/* Birthdays from every AddressBook-v22.abcddb under `addressbook_dir`
 * (the top level and Sources/<uuid>/), opened read-only. A database that
 * cannot be opened (e.g. no Contacts permission) is skipped, and
 * *out_unreadable counts it, so "no birthdays" and "could not look" differ. */
hu_error_t hu_addressbook_birthdays(const char *addressbook_dir, hu_addressbook_birthday_t *out,
                                    size_t cap, size_t *out_n, size_t *out_unreadable);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_PERSON_DATES_REPO_H */
