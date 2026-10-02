/* Send-provenance observer: every delivered outbound iMessage (text, media,
 * threaded reply) is reported exactly once, with the final text, and nothing
 * is reported for a send that did not happen. The offline conversation-quality
 * metric (scripts/eval_conversation_quality.py) relies on this to tell
 * h-uman's sends from Seth's own. */
#include "human/channels/imessage_reply.h"
#include "human/channels/imessage_send_observer.h"
#include "test_framework.h"
#include <string.h>
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif

typedef struct {
    int calls;
    char handle[64];
    char text[256];
    char kind[16];
    int64_t prior;
    void *user_seen;
} capture_t;

static capture_t g_cap;

static void capture_cb(void *user, const hu_imessage_sent_event_t *ev) {
    g_cap.calls++;
    g_cap.user_seen = user;
    size_t hl =
        ev->handle_len < sizeof(g_cap.handle) - 1 ? ev->handle_len : sizeof(g_cap.handle) - 1;
    memcpy(g_cap.handle, ev->handle, hl);
    g_cap.handle[hl] = '\0';
    size_t tl = ev->text_len < sizeof(g_cap.text) - 1 ? ev->text_len : sizeof(g_cap.text) - 1;
    if (ev->text && tl > 0)
        memcpy(g_cap.text, ev->text, tl);
    g_cap.text[tl] = '\0';
    snprintf(g_cap.kind, sizeof(g_cap.kind), "%s", ev->kind ? ev->kind : "");
    g_cap.prior = ev->prior_max_rowid;
}

static void reset(void) {
    memset(&g_cap, 0, sizeof(g_cap));
    hu_imessage_send_observer_set(capture_cb, &g_cap);
}

static void send_observer_notify_delivers_event_to_registered_observer(void) {
    reset();
    hu_imessage_sent_event_t ev = {.handle = "+15550001111",
                                   .handle_len = 12,
                                   .text = "hey there",
                                   .text_len = 9,
                                   .kind = HU_IMESSAGE_SENT_KIND_TEXT,
                                   .prior_max_rowid = 41};
    hu_imessage_send_observer_notify(&ev);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_STR_EQ(g_cap.handle, "+15550001111");
    HU_ASSERT_STR_EQ(g_cap.text, "hey there");
    HU_ASSERT_STR_EQ(g_cap.kind, "text");
    HU_ASSERT_EQ(g_cap.prior, 41);
    HU_ASSERT_TRUE(g_cap.user_seen == &g_cap);
    hu_imessage_send_observer_set(NULL, NULL);
}

static void send_observer_notify_is_noop_when_unset(void) {
    reset();
    hu_imessage_send_observer_set(NULL, NULL);
    HU_ASSERT_FALSE(hu_imessage_send_observer_active());
    hu_imessage_sent_event_t ev = {.handle = "+1", .handle_len = 2, .kind = "text"};
    hu_imessage_send_observer_notify(&ev);
    HU_ASSERT_EQ(g_cap.calls, 0);
}

static void send_observer_notify_ignores_null_and_empty_handle(void) {
    reset();
    hu_imessage_send_observer_notify(NULL);
    hu_imessage_sent_event_t ev = {.handle = "", .handle_len = 0, .kind = "text"};
    hu_imessage_send_observer_notify(&ev);
    HU_ASSERT_EQ(g_cap.calls, 0);
    hu_imessage_send_observer_set(NULL, NULL);
}

static bool tier1_ok(const char *guid, size_t guid_len, const char *body, size_t body_len) {
    (void)guid;
    (void)guid_len;
    (void)body;
    (void)body_len;
    return true;
}

static bool tier_fail(const char *guid, size_t guid_len, const char *body, size_t body_len) {
    (void)guid;
    (void)guid_len;
    (void)body;
    (void)body_len;
    return false;
}

static hu_error_t flat_fail(const char *target, size_t target_len, const char *body,
                            size_t body_len) {
    (void)target;
    (void)target_len;
    (void)body;
    (void)body_len;
    return HU_ERR_CHANNEL_SEND;
}

static void threaded_reply_success_reports_reply_event(void) {
    reset();
    hu_imessage_set_test_reply_stubs(tier1_ok, tier_fail, NULL);
    HU_ASSERT_EQ(hu_imessage_reply(NULL, "+15555551212", 12, "PARENT-GUID", 11, "on my way", 9),
                 HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_STR_EQ(g_cap.kind, "reply");
    HU_ASSERT_STR_EQ(g_cap.handle, "+15555551212");
    HU_ASSERT_STR_EQ(g_cap.text, "on my way");
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_send_observer_set(NULL, NULL);
}

static void threaded_reply_failure_reports_nothing(void) {
    reset();
    hu_imessage_set_test_reply_stubs(tier_fail, tier_fail, flat_fail);
    HU_ASSERT_TRUE(hu_imessage_reply(NULL, "+15555551212", 12, "PARENT-GUID", 11, "hi", 2) !=
                   HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 0);
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_send_observer_set(NULL, NULL);
}

#if HU_HAS_IMESSAGE
static void channel_text_send_reports_final_text_once(void) {
    reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15551234567", 12, "see you soon", 12, NULL, 0), HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_STR_EQ(g_cap.kind, "text");
    HU_ASSERT_STR_EQ(g_cap.handle, "+15551234567");
    HU_ASSERT_STR_EQ(g_cap.text, "see you soon");
    hu_imessage_destroy(&ch);
    hu_imessage_send_observer_set(NULL, NULL);
}

static void channel_media_only_send_reports_media_kind(void) {
    reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    const char *media[] = {"/tmp/voice.m4a"};
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15551234567", 12, "", 0, media, 1), HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_STR_EQ(g_cap.kind, "media");
    hu_imessage_destroy(&ch);
    hu_imessage_send_observer_set(NULL, NULL);
}

/* A threaded reply is an outbound text like any other: on a self-chat (the
 * owner's #text tests) every bubble the twin sends also arrives as an inbound
 * row, and only the echo ring stops it being answered. 2026-09-30: the
 * threaded path never recorded, so "hung out by the water" came back, got a
 * reply ("peaceful"), which came back too - a loop on the owner's number. */
static void channel_threaded_reply_is_remembered_as_our_echo(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_test_reply_stubs(tier1_ok, tier_fail, NULL);
    HU_ASSERT_FALSE(hu_imessage_test_in_echo_ring(&ch, "hung out by the water", 21));
    HU_ASSERT_EQ(ch.vtable->reply(ch.ctx, "+15551234567", 12, "PARENT-GUID", 11,
                                  "hung out by the water", 21),
                 HU_OK);
    HU_ASSERT_TRUE(hu_imessage_test_in_echo_ring(&ch, "hung out by the water", 21));
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_destroy(&ch);
}

static void channel_failed_threaded_reply_is_not_an_echo(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_test_reply_stubs(tier_fail, tier_fail, flat_fail);
    HU_ASSERT_TRUE(ch.vtable->reply(ch.ctx, "+15551234567", 12, "PARENT-GUID", 11, "peaceful", 8) !=
                   HU_OK);
    HU_ASSERT_FALSE(hu_imessage_test_in_echo_ring(&ch, "peaceful", 8));
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_destroy(&ch);
}

/* Tapbacks are sends too: the behaviour learner (scripts/learned_style_v2.py)
 * reads outbound_sends kind 'tapback' to drop the twin's own reactions and
 * learn only Seth's. One report per delivered tapback, no text. */
static void channel_react_reports_tapback_once(void) {
    reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 0);
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15551234567", 12, 42, HU_REACTION_HEART), HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_STR_EQ(g_cap.kind, "tapback");
    HU_ASSERT_STR_EQ(g_cap.handle, "+15551234567");
    HU_ASSERT_STR_EQ(g_cap.text, "");
    hu_imessage_destroy(&ch);
    hu_imessage_send_observer_set(NULL, NULL);
}

static void channel_failed_react_reports_nothing(void) {
    reset();
    HU_ASSERT_TRUE(hu_imessage_react_emoji_with_fallback(NULL, "+15551234567", 12, 42, "", 0) !=
                   HU_OK);
    hu_channel_t ch;
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_TRUE(ch.vtable->react(NULL, "+15551234567", 12, 42, HU_REACTION_HEART) != HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 0);
    hu_imessage_destroy(&ch);
    hu_imessage_send_observer_set(NULL, NULL);
}

/* The #603 learner (claim_bot_tapbacks) claims the first from-me tapback
 * ABOVE prior_max_rowid, within a window that ends shortly after the record
 * time. So the boundary must be read before the react goes out (or it would
 * already include the tapback and claim nothing), and the record must be
 * made after the react succeeded (or it would predate the tapback). */
static hu_channel_t *g_order_ch;
static hu_reaction_type_t g_reaction_at_boundary;
static hu_reaction_type_t g_reaction_at_notify;
static int g_boundary_reads;

static int64_t order_boundary_stub(const char *handle, size_t handle_len) {
    (void)handle;
    (void)handle_len;
    int64_t mid = 0;
    g_boundary_reads++;
    hu_imessage_test_get_last_reaction(g_order_ch, &g_reaction_at_boundary, &mid);
    return 777;
}

static void order_capture_cb(void *user, const hu_imessage_sent_event_t *ev) {
    int64_t mid = 0;
    hu_imessage_test_get_last_reaction(g_order_ch, &g_reaction_at_notify, &mid);
    capture_cb(user, ev);
}

static void channel_react_boundary_before_send_record_after(void) {
    memset(&g_cap, 0, sizeof(g_cap));
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    g_order_ch = &ch;
    g_boundary_reads = 0;
    g_reaction_at_boundary = HU_REACTION_QUESTION;
    g_reaction_at_notify = HU_REACTION_QUESTION;
    hu_imessage_set_test_tapback_boundary_stub(order_boundary_stub);
    hu_imessage_send_observer_set(order_capture_cb, &g_cap);

    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15551234567", 12, 42, HU_REACTION_HEART), HU_OK);
    HU_ASSERT_EQ(g_boundary_reads, 1);
    /* Boundary read while the reaction had not been applied yet... */
    HU_ASSERT_EQ((int)g_reaction_at_boundary, (int)HU_REACTION_NONE);
    /* ...and the record made once it had. */
    HU_ASSERT_EQ((int)g_reaction_at_notify, (int)HU_REACTION_HEART);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_EQ(g_cap.prior, 777);

    hu_imessage_set_test_tapback_boundary_stub(NULL);
    hu_imessage_send_observer_set(NULL, NULL);
    hu_imessage_destroy(&ch);
}

static bool picker_ok(const char *emoji_utf8) {
    (void)emoji_utf8;
    return true;
}

static bool picker_miss(const char *emoji_utf8) {
    (void)emoji_utf8;
    return false;
}

static void channel_react_emoji_reports_tapback_once(void) {
    reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_test_react_emoji_stub(picker_ok);
    HU_ASSERT_EQ(ch.vtable->react_emoji(ch.ctx, "+15551234567", 12, 42, "\xF0\x9F\x91\x8D", 4),
                 HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 1);
    HU_ASSERT_STR_EQ(g_cap.kind, "tapback");
    HU_ASSERT_STR_EQ(g_cap.handle, "+15551234567");
    HU_ASSERT_STR_EQ(g_cap.text, "");
    /* A picker miss in a test build falls through to NOT_SUPPORTED: no report. */
    hu_imessage_set_test_react_emoji_stub(picker_miss);
    HU_ASSERT_TRUE(ch.vtable->react_emoji(ch.ctx, "+15551234567", 12, 42, "\xF0\x9F\x91\x8D", 4) !=
                   HU_OK);
    HU_ASSERT_EQ(g_cap.calls, 1);
    hu_imessage_set_test_react_emoji_stub(NULL);
    hu_imessage_destroy(&ch);
    hu_imessage_send_observer_set(NULL, NULL);
}

/* End-to-end through the daemon recorder into a real (in-memory) memory.db. */
#if defined(HU_ENABLE_SQLITE) && HU_HAS_IMESSAGE
#include "human/core/time.h"
#include "human/daemon/send_provenance.h"
#include "human/memory.h"
#include <sqlite3.h>

/* First column of the first row of `sql`, or -1. */
static int64_t prov_scalar(sqlite3 *db, const char *sql) {
    int64_t v = -1;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
        v = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return v;
}

static sqlite3 *open_mem(hu_memory_t *mem, hu_allocator_t *alloc) {
    *mem = hu_sqlite_memory_create(alloc, ":memory:");
    return mem->ctx ? hu_sqlite_memory_get_db(mem) : NULL;
}
static int64_t boundary_4242(const char *handle, size_t handle_len) {
    (void)handle;
    (void)handle_len;
    return 4242;
}

/* End to end: a tapback the channel delivers lands in outbound_sends as
 * kind 'tapback', no text, the recipient handle and the chat.db boundary. */
static void send_provenance_records_channel_tapback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    sqlite3 *db = open_mem(&mem, &alloc);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_daemon_send_provenance_install(db), HU_OK);
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(prov_scalar(db, "SELECT COUNT(*) FROM outbound_sends"), 0);
    hu_imessage_set_test_tapback_boundary_stub(boundary_4242);
    int64_t wall_before = hu_time_wall_ms();
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15551234567", 12, 314, HU_REACTION_HEART), HU_OK);
    int64_t wall_after = hu_time_wall_ms();
    hu_imessage_set_test_tapback_boundary_stub(NULL);
    HU_ASSERT_EQ(prov_scalar(db, "SELECT COUNT(*) FROM outbound_sends WHERE kind='tapback' AND "
                                 "contact='+15551234567' AND channel='imessage' AND "
                                 "text IS NULL AND prior_max_rowid=4242"),
                 1);
    /* Wall-clock ms stamped at the record, not before the react began. */
    HU_ASSERT_TRUE(prov_scalar(db, "SELECT sent_at_ms FROM outbound_sends") >= wall_before);
    HU_ASSERT_TRUE(prov_scalar(db, "SELECT sent_at_ms FROM outbound_sends") <= wall_after);
    hu_imessage_destroy(&ch);
    hu_daemon_send_provenance_uninstall();
    mem.vtable->deinit(mem.ctx);
}

/* Bookkeeping never blocks a tapback: with memory.db refusing writes the
 * react still succeeds and the reaction was still applied. */
static void send_provenance_write_failure_does_not_fail_react(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    sqlite3 *db = open_mem(&mem, &alloc);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(hu_daemon_send_provenance_install(db), HU_OK);
    HU_ASSERT_EQ(sqlite3_exec(db, "PRAGMA query_only = 1;", NULL, NULL, NULL), SQLITE_OK);
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15551234567", 12, 99, HU_REACTION_HAHA), HU_OK);
    hu_reaction_type_t r = HU_REACTION_NONE;
    int64_t mid = 0;
    hu_imessage_test_get_last_reaction(&ch, &r, &mid);
    HU_ASSERT_EQ((int)r, (int)HU_REACTION_HAHA);
    HU_ASSERT_EQ(mid, 99);
    HU_ASSERT_EQ(sqlite3_exec(db, "PRAGMA query_only = 0;", NULL, NULL, NULL), SQLITE_OK);
    HU_ASSERT_EQ(prov_scalar(db, "SELECT COUNT(*) FROM outbound_sends"), 0);
    hu_imessage_destroy(&ch);
    hu_daemon_send_provenance_uninstall();
    mem.vtable->deinit(mem.ctx);
}
#endif

static void channel_rejected_send_reports_nothing(void) {
    reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15551234567", 12, "", 0, NULL, 0),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(g_cap.calls, 0);
    hu_imessage_destroy(&ch);
    hu_imessage_send_observer_set(NULL, NULL);
}
#endif

void run_imessage_send_observer_tests(void) {
    HU_TEST_SUITE("imessage_send_observer");
    HU_RUN_TEST(send_observer_notify_delivers_event_to_registered_observer);
    HU_RUN_TEST(send_observer_notify_is_noop_when_unset);
    HU_RUN_TEST(send_observer_notify_ignores_null_and_empty_handle);
    HU_RUN_TEST(threaded_reply_success_reports_reply_event);
    HU_RUN_TEST(threaded_reply_failure_reports_nothing);
#if HU_HAS_IMESSAGE
    HU_RUN_TEST(channel_text_send_reports_final_text_once);
    HU_RUN_TEST(channel_media_only_send_reports_media_kind);
    HU_RUN_TEST(channel_rejected_send_reports_nothing);
    HU_RUN_TEST(channel_threaded_reply_is_remembered_as_our_echo);
    HU_RUN_TEST(channel_failed_threaded_reply_is_not_an_echo);
    HU_RUN_TEST(channel_react_reports_tapback_once);
    HU_RUN_TEST(channel_failed_react_reports_nothing);
    HU_RUN_TEST(channel_react_emoji_reports_tapback_once);
    HU_RUN_TEST(channel_react_boundary_before_send_record_after);
#if defined(HU_ENABLE_SQLITE)
    HU_RUN_TEST(send_provenance_records_channel_tapback);
    HU_RUN_TEST(send_provenance_write_failure_does_not_fail_react);
#endif
#endif
}
