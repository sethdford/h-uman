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
#endif
}
