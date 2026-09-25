/* Exercises hu_outbound_sends_repo_* (src/memory/repos/outbound_sends_repo_sqlite.c)
 * and hu_daemon_send_provenance_{install,uninstall}
 * (src/daemon/daemon_send_provenance.c) end to end through the iMessage
 * send observer.
 * @covers-none — scripts/check-test-references.sh strips "outbound_sends_repo"
 * down to "outbound" and can match unrelated outbound sources; the real
 * production files are named above. */
#ifdef HU_ENABLE_SQLITE
#include "human/channels/imessage_send_observer.h"
#include "human/daemon/send_provenance.h"
#include "human/memory.h"
#include "human/memory/outbound_sends_repo.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>

static sqlite3 *open_mem(hu_memory_t *mem, hu_allocator_t *alloc) {
    *mem = hu_sqlite_memory_create(alloc, ":memory:");
    return mem->ctx ? hu_sqlite_memory_get_db(mem) : NULL;
}

static void outbound_sends_repo_record_and_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    sqlite3 *db = open_mem(&mem, &alloc);
    HU_ASSERT_NOT_NULL(db);
    int64_t n = -1;
    HU_ASSERT_EQ(hu_outbound_sends_repo_count(db, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(db, 1000, "imessage", "+15550001111", 12,
                                               HU_OUTBOUND_SEND_KIND_TEXT, "hey", 3, 77),
                 HU_OK);
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(db, 1001, "imessage", "+15550001111", 12,
                                               HU_OUTBOUND_SEND_KIND_MEDIA, NULL, 0, -1),
                 HU_OK);
    HU_ASSERT_EQ(hu_outbound_sends_repo_count(db, &n), HU_OK);
    HU_ASSERT_EQ(n, 2);

    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT contact, kind, text, prior_max_rowid FROM "
                                    "outbound_sends ORDER BY id LIMIT 1",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), "+15550001111");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "text");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 2), "hey");
    HU_ASSERT_EQ(sqlite3_column_int64(st, 3), 77);
    sqlite3_finalize(st);
    mem.vtable->deinit(mem.ctx);
}

static void outbound_sends_repo_rejects_invalid_kind_and_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    sqlite3 *db = open_mem(&mem, &alloc);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(db, 1, "imessage", "+1", 2, "tapback", "x", 1, -1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(db, 1, "imessage", "", 0, "text", "x", 1, -1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(NULL, 1, "imessage", "+1", 2, "text", "x", 1, -1),
                 HU_ERR_INVALID_ARGUMENT);
    int64_t n = -1;
    HU_ASSERT_EQ(hu_outbound_sends_repo_count(db, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    mem.vtable->deinit(mem.ctx);
}

static void outbound_sends_repo_contact_not_nul_terminated(void) {
    /* Channel handles arrive as (ptr, len) and are not guaranteed to be
     * NUL-terminated; only contact_len bytes may be stored. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    sqlite3 *db = open_mem(&mem, &alloc);
    HU_ASSERT_NOT_NULL(db);
    const char buf[] = "+15550001111TRAILINGJUNK";
    HU_ASSERT_EQ(hu_outbound_sends_repo_record(db, 5, "imessage", buf, 12, "text", "hi", 2, -1),
                 HU_OK);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT contact FROM outbound_sends", -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), "+15550001111");
    sqlite3_finalize(st);
    mem.vtable->deinit(mem.ctx);
}

static void send_provenance_install_records_observed_sends(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    sqlite3 *db = open_mem(&mem, &alloc);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_daemon_send_provenance_install(db), HU_OK);
    HU_ASSERT_TRUE(hu_imessage_send_observer_active());

    hu_imessage_sent_event_t ev = {.handle = "+15550002222",
                                   .handle_len = 12,
                                   .text = "running late",
                                   .text_len = 12,
                                   .kind = HU_IMESSAGE_SENT_KIND_REPLY,
                                   .prior_max_rowid = 900};
    hu_imessage_send_observer_notify(&ev);

    int64_t n = -1;
    HU_ASSERT_EQ(hu_outbound_sends_repo_count(db, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT channel, contact, kind, text, prior_max_rowid, "
                                    "sent_at_ms FROM outbound_sends",
                                    -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 0), "imessage");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 1), "+15550002222");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 2), "reply");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(st, 3), "running late");
    HU_ASSERT_EQ(sqlite3_column_int64(st, 4), 900);
    HU_ASSERT_TRUE(sqlite3_column_int64(st, 5) > 0);
    sqlite3_finalize(st);

    hu_daemon_send_provenance_uninstall();
    HU_ASSERT_FALSE(hu_imessage_send_observer_active());
    hu_imessage_send_observer_notify(&ev); /* after uninstall: nothing recorded */
    HU_ASSERT_EQ(hu_outbound_sends_repo_count(db, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    mem.vtable->deinit(mem.ctx);
}

static void send_provenance_install_rejects_null_db(void) {
    HU_ASSERT_EQ(hu_daemon_send_provenance_install(NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(hu_imessage_send_observer_active());
}

void run_outbound_sends_repo_tests(void) {
    HU_TEST_SUITE("outbound_sends_repo");
    HU_RUN_TEST(outbound_sends_repo_record_and_count);
    HU_RUN_TEST(outbound_sends_repo_rejects_invalid_kind_and_contact);
    HU_RUN_TEST(outbound_sends_repo_contact_not_nul_terminated);
    HU_RUN_TEST(send_provenance_install_records_observed_sends);
    HU_RUN_TEST(send_provenance_install_rejects_null_db);
}
#else
void run_outbound_sends_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
