#include "human/context/self_awareness.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/memory/engines.h"
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

/* P2-5 regression (2026-05-16 incident): hu_daemon_build_callback_context
 * memcpy'd raw entries[i].content bytes into the callback prompt. Memory
 * entries containing first-person confession-style content thus leaked
 * directly into proactive prompts AND through to outbound messages.
 *
 * Pin the safety predicate directly — these tests must FAIL if the
 * predicate ever regresses to letting unsafe content through.  Per
 * .claude/rules/security-predicate-extraction.md the decision is its own
 * unit-testable function. */
static void test_callback_content_is_safe_accepts_clean_content(void) {
    HU_ASSERT_TRUE(hu_daemon_callback_content_is_safe("user wanted to try a pasta recipe", 33));
    HU_ASSERT_TRUE(hu_daemon_callback_content_is_safe("project deadline next week", 26));
    HU_ASSERT_TRUE(hu_daemon_callback_content_is_safe("favorite coffee shop nearby", 27));
}

static void test_callback_content_is_safe_rejects_first_person(void) {
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("I confessed something terrible", 30));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("i'm learning to lie better", 26));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("we talked about my secret", 25));
}

static void test_callback_content_is_safe_rejects_confession_verbs(void) {
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("they admitted what happened", 27));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("user betrayed a trust", 21));
}

static void test_callback_content_is_safe_rejects_charged_keywords(void) {
    HU_ASSERT_FALSE(
        hu_daemon_callback_content_is_safe("feeling lonely and depressed every morning", 42));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("user looks miserable lately", 27));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("scared of what comes next", 25));
}

static void test_callback_content_is_safe_rejects_format_injection(void) {
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("benign %s injection", 19));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("multi\nline content", 18));
}

static void test_callback_content_is_safe_handles_null_and_empty(void) {
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe(NULL, 0));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe(NULL, 10));
    HU_ASSERT_FALSE(hu_daemon_callback_content_is_safe("", 0));
}

static void test_build_callback_context_skips_confession_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char SESSION[] = "session_p25";
    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };
    const char *key = "topic:session_p25:confession";
    const char *content = "I confessed something terrible to my friend last week";
    mem.vtable->store(mem.ctx, key, strlen(key), content, strlen(content), &cat, SESSION,
                      sizeof(SESSION) - 1);

    size_t out_len = 0;
    char *result = hu_daemon_build_callback_context(&alloc, &mem, SESSION, sizeof(SESSION) - 1,
                                                    "hello", 5, &out_len, NULL);

    /* If a context was built, it MUST NOT contain the confession fragment. */
    if (result && out_len > 0) {
        HU_ASSERT_NULL(strstr(result, "confessed something terrible"));
        HU_ASSERT_NULL(strstr(result, "I confessed"));
        alloc.free(alloc.ctx, result, out_len + 1);
    }
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

/* P2-11 regression (2026-05-16 incident): memory degradation was being
 * applied to content destined for an OUTBOUND proactive prompt — which
 * meant the LLM saw corrupted text and could ship it verbatim to
 * contacts. Degradation is a UX-of-recall concept; it should not corrupt
 * outbound-prompt content. Verify that clean content passes through this
 * function bit-perfect (no random char swaps). */
static void test_build_callback_context_does_not_apply_degradation_to_outbound(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char SESSION[] = "session_p11";
    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };
    /* A very distinctive clean memory entry. */
    const char *key = "topic:session_p11:distinct";
    const char *content = "user mentioned the artisan-pasta workshop on Saturday";
    mem.vtable->store(mem.ctx, key, strlen(key), content, strlen(content), &cat, SESSION,
                      sizeof(SESSION) - 1);

    size_t out_len = 0;
    char *result = hu_daemon_build_callback_context(&alloc, &mem, SESSION, sizeof(SESSION) - 1,
                                                    "hello", 5, &out_len, NULL);

    /* If a context was built, the distinctive content must appear bit-
     * perfect — degradation would corrupt characters, breaking the
     * substring match. */
    if (result && out_len > 0) {
        HU_ASSERT_NOT_NULL(strstr(result, "artisan-pasta workshop on Saturday"));
        alloc.free(alloc.ctx, result, out_len + 1);
    }
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void test_build_callback_context_skips_emotion_keyword_entries(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char SESSION[] = "session_p25b";
    static const char topic_cat[] = "conversation";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = topic_cat, .name_len = sizeof(topic_cat) - 1},
    };
    const char *key = "topic:session_p25b:lonely";
    const char *content = "feeling lonely and depressed every morning";
    mem.vtable->store(mem.ctx, key, strlen(key), content, strlen(content), &cat, SESSION,
                      sizeof(SESSION) - 1);

    size_t out_len = 0;
    char *result = hu_daemon_build_callback_context(&alloc, &mem, SESSION, sizeof(SESSION) - 1,
                                                    "hello", 5, &out_len, NULL);

    if (result && out_len > 0) {
        HU_ASSERT_NULL(strstr(result, "lonely"));
        HU_ASSERT_NULL(strstr(result, "depressed"));
        alloc.free(alloc.ctx, result, out_len + 1);
    }
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

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
    HU_RUN_TEST(test_callback_content_is_safe_accepts_clean_content);
    HU_RUN_TEST(test_callback_content_is_safe_rejects_first_person);
    HU_RUN_TEST(test_callback_content_is_safe_rejects_confession_verbs);
    HU_RUN_TEST(test_callback_content_is_safe_rejects_charged_keywords);
    HU_RUN_TEST(test_callback_content_is_safe_rejects_format_injection);
    HU_RUN_TEST(test_callback_content_is_safe_handles_null_and_empty);
    HU_RUN_TEST(test_build_callback_context_skips_confession_entries);
    HU_RUN_TEST(test_build_callback_context_does_not_apply_degradation_to_outbound);
    HU_RUN_TEST(test_build_callback_context_skips_emotion_keyword_entries);
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
