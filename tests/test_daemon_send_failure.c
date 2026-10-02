/* A text send that failed on every path is never lost silently (2026-09-26:
 * a reply to an RCS contact failed three times and left no trace but a debug
 * line). Exercises src/daemon/daemon_send_failure.c against an in-memory
 * memory.db: the send_failed row the proactive pass reads, the WARN counter,
 * the owner notification (rate-limited, never quoting the message), and the
 * registration through hu_daemon_send_provenance_install. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE
#include "human/channels/imessage_send_observer.h"
#include "human/daemon/owner_notify.h"
#include "human/daemon/send_failure.h"
#include "human/daemon/send_provenance.h"
#include "human/memory.h"
#include "human/memory/outbound_sends_repo.h"
#include <sqlite3.h>
#include <string.h>

#define H     "+15550001111"
#define H_LEN 12

static int64_t failed_rows(sqlite3 *db, const char *contact) {
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM proactive_decisions WHERE contact = ?1 "
                           "AND trigger = 'outbound_send' AND reason = 'send_failed' AND sent = 0",
                           -1, &st, NULL) != SQLITE_OK)
        return -2; /* table absent */
    sqlite3_bind_text(st, 1, contact, -1, NULL);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static hu_imessage_send_failed_event_t ev_for(const char *h, size_t hl) {
    hu_imessage_send_failed_event_t ev = {.handle = h,
                                          .handle_len = hl,
                                          .chat_service = "RCS",
                                          .text = "ugh that sucks, hope you feel better",
                                          .text_len = 36};
    return ev;
}

static void send_failure_record_writes_row_and_counts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    hu_owner_notify_test_reset();
    uint64_t before = hu_daemon_send_failure_total();
    HU_ASSERT_TRUE(failed_rows(db, H) <= 0);

    hu_imessage_send_failed_event_t ev = ev_for(H, H_LEN);
    hu_daemon_send_failure_record(db, &ev, 1000);

    HU_ASSERT_EQ(failed_rows(db, H), 1);
    HU_ASSERT_EQ(hu_daemon_send_failure_total(), before + 1);
    mem.vtable->deinit(mem.ctx);
}

static void send_failure_notifies_owner_once_per_window_without_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_owner_notify_test_reset();
    hu_daemon_send_failure_test_reset();

    hu_imessage_send_failed_event_t ev = ev_for(H, H_LEN);
    hu_daemon_send_failure_record(db, &ev, 1000);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
    const char *body = hu_owner_notify_test_last_body();
    HU_ASSERT_NOT_NULL(strstr(body, "1111"));
    HU_ASSERT_NOT_NULL(strstr(body, "RCS"));
    HU_ASSERT_NULL(strstr(body, "hope you feel better")); /* never quotes the message */

    /* The other bubbles of the same reply fail seconds later: one banner. */
    hu_daemon_send_failure_record(db, &ev, 1140);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
    /* ...but every failure is still recorded. */
    HU_ASSERT_EQ(failed_rows(db, H), 2);

    /* A different contact, or the same one after the window, notifies again. */
    hu_imessage_send_failed_event_t ev2 = ev_for("+15550002222", 12);
    hu_daemon_send_failure_record(db, &ev2, 1150);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 2u);
    hu_daemon_send_failure_record(db, &ev, 1000 + 601);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 3u);
    mem.vtable->deinit(mem.ctx);
}

static void send_failure_last_undelivered_until_a_later_delivery(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t ts = 0;
    HU_ASSERT_FALSE(hu_daemon_send_failure_last_undelivered(db, H, &ts));

    hu_imessage_send_failed_event_t ev = ev_for(H, H_LEN);
    hu_daemon_send_failure_record(db, &ev, 1000);
    HU_ASSERT_TRUE(hu_daemon_send_failure_last_undelivered(db, H, &ts));
    HU_ASSERT_EQ(ts, 1000);
    HU_ASSERT_FALSE(hu_daemon_send_failure_last_undelivered(db, "+15550009999", &ts));

    /* A later delivered send to the same contact closes it. */
    HU_ASSERT_EQ(
        hu_outbound_sends_repo_record(db, 2000 * 1000LL, "imessage", H, H_LEN, "text", "ok", 2, -1),
        HU_OK);
    HU_ASSERT_FALSE(hu_daemon_send_failure_last_undelivered(db, H, &ts));
    mem.vtable->deinit(mem.ctx);
}

static void send_failure_observer_is_registered_by_provenance_install(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_imessage_send_failed_event_t ev = ev_for(H, H_LEN);

    /* Not installed: a notify reaches nobody. */
    hu_daemon_send_provenance_uninstall();
    hu_imessage_send_failure_notify(&ev);
    HU_ASSERT_TRUE(failed_rows(db, H) <= 0);

    HU_ASSERT_EQ(hu_daemon_send_provenance_install(db), HU_OK);
    hu_imessage_send_failure_notify(&ev);
    HU_ASSERT_EQ(failed_rows(db, H), 1);

    hu_daemon_send_provenance_uninstall();
    hu_imessage_send_failure_notify(&ev);
    HU_ASSERT_EQ(failed_rows(db, H), 1);
    mem.vtable->deinit(mem.ctx);
}

void run_daemon_send_failure_tests(void) {
    HU_TEST_SUITE("daemon_send_failure");
    HU_RUN_TEST(send_failure_record_writes_row_and_counts);
    HU_RUN_TEST(send_failure_notifies_owner_once_per_window_without_text);
    HU_RUN_TEST(send_failure_last_undelivered_until_a_later_delivery);
    HU_RUN_TEST(send_failure_observer_is_registered_by_provenance_install);
}

#else
void run_daemon_send_failure_tests(void) {
    (void)0;
}
#endif
