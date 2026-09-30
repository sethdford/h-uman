/* Exercises hu_person_dates_repo_* and hu_addressbook_birthdays in
 * src/memory/repos/person_dates_repo_sqlite.c (life-admin slice 4).
 * @covers-none — the production file is named above; "person_dates_repo" has
 * no same-named source for the basename heuristic to find.
 */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/person_dates_repo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_person_dates_repo_set_replaces_and_lists_by_date(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "+15550000002", "birthday", 3, 10, "t", 1), HU_OK);
    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "+15550000001", "birthday", 3, 2, "t", 1), HU_OK);
    /* Same person and label: the later statement wins, no duplicate row. */
    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "+15550000002", "birthday", 3, 3, "t", 2), HU_OK);
    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "+15550000002", "anniversary", 6, 12, "t", 2), HU_OK);
    hu_person_date_t rows[8];
    size_t n = 0;
    HU_ASSERT_EQ(hu_person_dates_repo_list(db, rows, 8, &n), HU_OK);
    HU_ASSERT_EQ(n, 3);
    HU_ASSERT_STR_EQ(rows[0].contact_id, "+15550000001");
    HU_ASSERT_EQ(rows[0].day, 2);
    HU_ASSERT_STR_EQ(rows[1].contact_id, "+15550000002");
    HU_ASSERT_EQ(rows[1].day, 3); /* replaced, not 10 */
    HU_ASSERT_STR_EQ(rows[2].label, "anniversary");

    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "+1", "birthday", 2, 30 + 2, "t", 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "+1", "birthday", 13, 1, "t", 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_person_dates_repo_set(db, "", "birthday", 1, 1, "t", 1),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

/* A minimal AddressBook-v22.abcddb: the two tables and columns read. */
static void make_addressbook(const char *path, const char *phone, long long core_data_birthday) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE ZABCDRECORD (Z_PK INTEGER PRIMARY KEY, ZBIRTHDAY REAL);"
             "CREATE TABLE ZABCDPHONENUMBER (ZOWNER INTEGER, ZFULLNUMBER TEXT);"
             "INSERT INTO ZABCDRECORD VALUES (1, %lld), (2, NULL);"
             "INSERT INTO ZABCDPHONENUMBER VALUES (1, '%s'), (2, '(555) 000-0007');",
             core_data_birthday, phone);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
}

static void test_addressbook_birthdays_reads_every_source_and_counts_unreadable(void) {
    char dir[] = "/tmp/hu_ab_test_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char path[256], src[256], src2[256];
    snprintf(src, sizeof(src), "%s/Sources", dir);
    HU_ASSERT_EQ(mkdir(src, 0700), 0);
    snprintf(src2, sizeof(src2), "%s/Sources/A1", dir);
    HU_ASSERT_EQ(mkdir(src2, 0700), 0);
    snprintf(path, sizeof(path), "%s/AddressBook-v22.abcddb", src2);
    /* 1985-03-02 12:00 GMT as Core Data stores it; a formatted US number. */
    make_addressbook(path, "+1 (555) 000-0001", -499694400LL);
    /* A top-level database that is not SQLite: counted, not fatal. */
    char bad[256];
    snprintf(bad, sizeof(bad), "%s/AddressBook-v22.abcddb", dir);
    FILE *f = fopen(bad, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("not a database, just bytes long enough to not be empty ......................", f);
    fclose(f);

    hu_addressbook_birthday_t out[8];
    size_t n = 0, unreadable = 0;
    HU_ASSERT_EQ(hu_addressbook_birthdays(dir, out, 8, &n, &unreadable), HU_OK);
    HU_ASSERT_EQ(n, 1); /* the card without a birthday is not listed */
    HU_ASSERT_STR_EQ(out[0].digits, "5550000001");
    HU_ASSERT_EQ(out[0].month, 3);
    HU_ASSERT_EQ(out[0].day, 2);
    HU_ASSERT_EQ(unreadable, 1);

    /* A directory with no Contacts databases is empty, not an error. */
    HU_ASSERT_EQ(hu_addressbook_birthdays("/nonexistent/ab", out, 8, &n, &unreadable), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(unreadable, 0);

    unlink(path);
    unlink(bad);
    rmdir(src2);
    rmdir(src);
    rmdir(dir);
}

void run_person_dates_repo_tests(void) {
    HU_TEST_SUITE("person dates repo");
    HU_RUN_TEST(test_person_dates_repo_set_replaces_and_lists_by_date);
    HU_RUN_TEST(test_addressbook_birthdays_reads_every_source_and_counts_unreadable);
}
#else
void run_person_dates_repo_tests(void) {}
#endif
