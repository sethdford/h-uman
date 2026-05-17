#include "human/context/self_awareness.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

/* Shared proactive context for all tests — reset before each test group. */
static hu_proactive_context_t g_test_ctx;

/* ── hu_daemon_channel_list_has_name ─────────────────────────────────── */

static const char *mock_channel_name(void *ctx) {
    return (const char *)ctx;
}

static void test_channel_list_has_name_null(void) {
    HU_ASSERT_FALSE(hu_daemon_channel_list_has_name(NULL, 0, "test"));
    HU_ASSERT_FALSE(hu_daemon_channel_list_has_name(NULL, 0, NULL));
}

static void test_channel_list_has_name_empty_name(void) {
    hu_service_channel_t ch[1];
    memset(ch, 0, sizeof(ch));
    HU_ASSERT_FALSE(hu_daemon_channel_list_has_name(ch, 1, ""));
    HU_ASSERT_FALSE(hu_daemon_channel_list_has_name(ch, 1, NULL));
}

static void test_channel_list_has_name_found(void) {
    hu_channel_vtable_t vt = {0};
    vt.name = mock_channel_name;
    hu_channel_t chan = {.ctx = (void *)"imessage", .vtable = &vt};
    hu_service_channel_t ch[1];
    memset(ch, 0, sizeof(ch));
    ch[0].channel = &chan;
    HU_ASSERT_TRUE(hu_daemon_channel_list_has_name(ch, 1, "imessage"));
}

static void test_channel_list_has_name_not_found(void) {
    hu_channel_vtable_t vt = {0};
    vt.name = mock_channel_name;
    hu_channel_t chan = {.ctx = (void *)"telegram", .vtable = &vt};
    hu_service_channel_t ch[1];
    memset(ch, 0, sizeof(ch));
    ch[0].channel = &chan;
    HU_ASSERT_FALSE(hu_daemon_channel_list_has_name(ch, 1, "imessage"));
}

/* ── hu_daemon_contact_activity_record / count / reset ───────────────── */

static void test_contact_activity_record_basic(void) {
    hu_proactive_context_reset(&g_test_ctx);
    HU_ASSERT_EQ(g_test_ctx.count, 0);

    hu_daemon_contact_activity_record(&g_test_ctx, "user_a", "imessage", "+1234567890");
    HU_ASSERT_EQ(g_test_ctx.count, 1);

    hu_daemon_contact_activity_record(&g_test_ctx, "user_b", "telegram", "user_b_tg");
    HU_ASSERT_EQ(g_test_ctx.count, 2);

    hu_proactive_context_reset(&g_test_ctx);
    HU_ASSERT_EQ(g_test_ctx.count, 0);
}

static void test_contact_activity_record_null(void) {
    hu_proactive_context_reset(&g_test_ctx);
    hu_daemon_contact_activity_record(&g_test_ctx, NULL, "imessage", "+1234567890");
    HU_ASSERT_EQ(g_test_ctx.count, 0);

    hu_daemon_contact_activity_record(&g_test_ctx, "user_a", NULL, "+1234567890");
    HU_ASSERT_EQ(g_test_ctx.count, 0);

    hu_daemon_contact_activity_record(&g_test_ctx, "user_a", "imessage", NULL);
    HU_ASSERT_EQ(g_test_ctx.count, 0);

    hu_daemon_contact_activity_record(&g_test_ctx, "", "imessage", "+1234567890");
    HU_ASSERT_EQ(g_test_ctx.count, 0);

    hu_proactive_context_reset(&g_test_ctx);
}

static void test_contact_activity_record_update_existing(void) {
    hu_proactive_context_reset(&g_test_ctx);
    hu_daemon_contact_activity_record(&g_test_ctx, "user_a", "imessage", "+1234567890");
    HU_ASSERT_EQ(g_test_ctx.count, 1);

    /* Same contact, different channel — should update, not add */
    hu_daemon_contact_activity_record(&g_test_ctx, "user_a", "telegram", "user_a_tg");
    HU_ASSERT_EQ(g_test_ctx.count, 1);

    hu_proactive_context_reset(&g_test_ctx);
}

/* ── hu_daemon_proactive_parse_route ─────────────────────────────────── */

static void test_parse_route_with_colon(void) {
    hu_contact_profile_t cp = {0};
    cp.proactive_channel = "imessage:+1234567890";
    cp.contact_id = "user_a";

    char ch[64], target[128];
    hu_daemon_proactive_parse_route(&cp, ch, target);

    HU_ASSERT_STR_EQ(ch, "imessage");
    HU_ASSERT_STR_EQ(target, "+1234567890");
}

static void test_parse_route_without_colon(void) {
    hu_contact_profile_t cp = {0};
    cp.proactive_channel = "telegram";
    cp.contact_id = "user_b_tg";

    char ch[64], target[128];
    hu_daemon_proactive_parse_route(&cp, ch, target);

    HU_ASSERT_STR_EQ(ch, "telegram");
    HU_ASSERT_STR_EQ(target, "user_b_tg");
}

/* ── hu_daemon_proactive_apply_route ─────────────────────────────────── */

static void test_apply_route_no_activity(void) {
    hu_proactive_context_reset(&g_test_ctx);
    char ch[64] = "imessage";
    char target[128] = "+1111111111";
    size_t len = strlen(target);

    hu_daemon_proactive_apply_route(&g_test_ctx, "user_x", time(NULL), NULL, 0, ch, target, &len);

    /* No activity recorded — should not change route */
    HU_ASSERT_STR_EQ(ch, "imessage");
    HU_ASSERT_STR_EQ(target, "+1111111111");

    hu_proactive_context_reset(&g_test_ctx);
}

static void test_apply_route_with_fresh_activity(void) {
    hu_proactive_context_reset(&g_test_ctx);

    /* Record activity on telegram */
    hu_daemon_contact_activity_record(&g_test_ctx, "user_a", "telegram", "user_a_tg");

    /* Set up a channel list that includes telegram */
    hu_channel_vtable_t vt = {0};
    vt.name = mock_channel_name;
    hu_channel_t chan = {.ctx = (void *)"telegram", .vtable = &vt};
    hu_service_channel_t channels[1];
    memset(channels, 0, sizeof(channels));
    channels[0].channel = &chan;

    char ch[64] = "imessage";
    char target[128] = "+1111111111";
    size_t len = strlen(target);

    hu_daemon_proactive_apply_route(&g_test_ctx, "user_a", time(NULL), channels, 1, ch, target,
                                    &len);

    /* Should override to telegram */
    HU_ASSERT_STR_EQ(ch, "telegram");
    HU_ASSERT_STR_EQ(target, "user_a_tg");

    hu_proactive_context_reset(&g_test_ctx);
}

/* ── hu_daemon_build_callback_context ────────────────────────────────── */

static void test_build_callback_context_null_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 999;
    char *result = hu_daemon_build_callback_context(&alloc, NULL, "s", 1, "msg", 3, &out_len, NULL);
    HU_ASSERT_NULL(result);
    HU_ASSERT_EQ(out_len, 0);
}

static void test_build_callback_context_null_msg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = {0};
    size_t out_len = 999;
    char *result = hu_daemon_build_callback_context(&alloc, &mem, "s", 1, NULL, 0, &out_len, NULL);
    HU_ASSERT_NULL(result);
    HU_ASSERT_EQ(out_len, 0);
}

/* ── 2026-05-16 P1-8: self-awareness directive must reach proactive prompt
 *
 * Audit: hu_self_awareness_build_directive(_from_memory) exists at
 * src/context/self_awareness.c:140-191 and 354-426, but no caller injected
 * its output into the proactive prompt assembly — which is why "how'd it go
 * with the loan?" fired 4 times to Mindy even with the directive
 * infrastructure present.
 *
 * This test simulates 3 consecutive same-topic sends (which makes
 * topic_repeat_count == 3) and then verifies the prompt built by
 * hu_daemon_proactive_prompt_for_contact contains the directive substring
 * "keep" — the same fingerprint pinned by tests/test_self_awareness.c::
 * self_awareness_directive_topic_repeat.
 * ─────────────────────────────────────────────────────────────────── */
#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"

static void test_p1_8_self_awareness_directive_reaches_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *contact = "mindy";
    /* Three sends on the same topic — drives topic_repeat_count to 3.
     * Recall hu_self_awareness_build_directive_from_memory: topic_repeat > 3
     * triggers the directive. We need 4 to exercise the "repeating" branch. */
    for (int i = 0; i < 4; i++)
        HU_ASSERT_EQ(hu_self_awareness_record_send(&alloc, &mem, contact, strlen(contact),
                                                   /*we_initiated=*/true, "loan", 4),
                     HU_OK);

    /* Pull the directive directly first to confirm it triggers. */
    char *dir = NULL;
    size_t dir_len = 0;
    HU_ASSERT_EQ(hu_self_awareness_build_directive_from_memory(
                     &alloc, &mem, contact, strlen(contact), (int64_t)time(NULL), &dir, &dir_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(dir_len > 0);
    /* Topic-repeat branch produces "I know I keep talking about <topic>" */
    HU_ASSERT_TRUE(strstr(dir, "keep") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "loan") != NULL);
    alloc.free(alloc.ctx, dir, dir_len + 1);

    /* Now build the actual proactive prompt and confirm the directive is
     * present in the output. The audit fix prepends the directive before the
     * base "You're initiating..." text. */
    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)contact;
    cp.name = (char *)"Mindy";

    size_t out_len = 0;
    char *prompt =
        hu_daemon_proactive_prompt_for_contact(&alloc, /*agent=*/NULL, &mem, &cp, &out_len);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "keep") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "loan") != NULL);
    alloc.free(alloc.ctx, prompt, out_len + 1);

    mem.vtable->deinit(mem.ctx);
}

static void test_p1_8_no_directive_when_no_repeat(void) {
    /* Negative control: a single send on a unique topic must NOT produce the
     * "I keep talking about" directive in the prompt. This guards against a
     * future change that fires the directive unconditionally. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *contact = "annie";
    HU_ASSERT_EQ(
        hu_self_awareness_record_send(&alloc, &mem, contact, strlen(contact), true, "garden", 6),
        HU_OK);

    hu_contact_profile_t cp = {0};
    cp.contact_id = (char *)contact;
    cp.name = (char *)"Annie";

    size_t out_len = 0;
    char *prompt =
        hu_daemon_proactive_prompt_for_contact(&alloc, /*agent=*/NULL, &mem, &cp, &out_len);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strstr(prompt, "I know I keep talking about") == NULL);
    alloc.free(alloc.ctx, prompt, out_len + 1);

    mem.vtable->deinit(mem.ctx);
}

#endif /* HU_ENABLE_SQLITE */

/* ── Test runner ─────────────────────────────────────────────────────── */

void run_daemon_proactive_tests(void) {
    HU_TEST_SUITE("daemon_proactive");

    /* channel list */
    HU_RUN_TEST(test_channel_list_has_name_null);
    HU_RUN_TEST(test_channel_list_has_name_empty_name);
    HU_RUN_TEST(test_channel_list_has_name_found);
    HU_RUN_TEST(test_channel_list_has_name_not_found);

    /* contact activity LRU */
    HU_RUN_TEST(test_contact_activity_record_basic);
    HU_RUN_TEST(test_contact_activity_record_null);
    HU_RUN_TEST(test_contact_activity_record_update_existing);

    /* route parsing */
    HU_RUN_TEST(test_parse_route_with_colon);
    HU_RUN_TEST(test_parse_route_without_colon);

    /* route application */
    HU_RUN_TEST(test_apply_route_no_activity);
    HU_RUN_TEST(test_apply_route_with_fresh_activity);

    /* callback context builder */
    HU_RUN_TEST(test_build_callback_context_null_memory);
    HU_RUN_TEST(test_build_callback_context_null_msg);

#ifdef HU_ENABLE_SQLITE
    /* 2026-05-16 P1-8 regression */
    HU_RUN_TEST(test_p1_8_self_awareness_directive_reaches_prompt);
    HU_RUN_TEST(test_p1_8_no_directive_when_no_repeat);
#endif
}
