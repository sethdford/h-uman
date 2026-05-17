#include "human/agent.h"
#include "human/core/string.h"
#include "human/daemon_proactive.h"
#include "human/humanness.h"
#include "human/memory.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

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

/* ── P6-1: channel overlay applied to proactive prompts ─────────────── */
/*
 * The reactive path (src/agent/agent_stream.c:512-558) calls
 * hu_persona_find_overlay and injects formality/avg_length/emoji_usage/
 * directness/face_saving/disagreement_style into the system prompt.
 * The proactive path historically did not. P6-1 closes that gap:
 * hu_daemon_proactive_prompt_for_contact must look up the overlay for
 * the contact's channel (parsed from cp->proactive_channel) and
 * inline its directives so the LLM receives the same per-channel
 * voice rules whether the turn was reactive or proactive.
 */
static void test_p6_1_proactive_prompt_includes_channel_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Build persona with a single imessage overlay using strdup so
     * hu_persona_deinit can free it via its standard path. */
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.name = hu_strndup(&alloc, "test", 4);
    persona.name_len = 4;

    persona.overlays = (hu_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(hu_persona_overlay_t));
    HU_ASSERT_NOT_NULL(persona.overlays);
    memset(persona.overlays, 0, sizeof(hu_persona_overlay_t));
    persona.overlays_count = 1;
    persona.overlays[0].channel = hu_strndup(&alloc, "imessage", 8);
    persona.overlays[0].formality = hu_strndup(&alloc, "casual", 6);
    persona.overlays[0].emoji_usage = hu_strndup(&alloc, "sparingly", 9);
    persona.overlays[0].avg_length = hu_strndup(&alloc, "short", 5);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.persona = &persona;

    hu_contact_profile_t cp = {0};
    cp.contact_id = "user_a";
    cp.name = "Alice";
    cp.proactive_channel = "imessage:+1234567890";
    cp.proactive_checkin = true;

    size_t out_len = 0;
    char *prompt = hu_daemon_proactive_prompt_for_contact(&alloc, &agent, NULL, &cp, &out_len);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(out_len > 0);

    /* The overlay's literal values must appear in the prompt. */
    HU_ASSERT_TRUE(strstr(prompt, "casual") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "sparingly") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "short") != NULL);

    alloc.free(alloc.ctx, prompt, out_len + 1);
    hu_persona_deinit(&alloc, &persona);
}

/* ── P6-2: relationship_type + dunbar_layer in proactive prompts ────── */
/*
 * Phase 6 audit: the proactive prompt used only cp->name and
 * cp->contact_id, so the LLM had no idea whether it was texting a
 * sister, a coworker, or a stranger from the gym. relationship_type
 * (family/friend/coworker/acquaintance) and dunbar_layer (1..5)
 * change the register that's appropriate for a check-in — these must
 * flow into the assembled prompt.
 */
static void test_p6_2_proactive_prompt_includes_relationship_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    persona.name = hu_strndup(&alloc, "test", 4);
    persona.name_len = 4;

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.persona = &persona;

    hu_contact_profile_t cp = {0};
    cp.contact_id = "user_sister";
    cp.name = "Maya";
    cp.proactive_channel = "imessage:+1234567890";
    cp.proactive_checkin = true;
    cp.relationship_type = "sister";
    cp.dunbar_layer = "1";

    size_t out_len = 0;
    char *prompt = hu_daemon_proactive_prompt_for_contact(&alloc, &agent, NULL, &cp, &out_len);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(out_len > 0);

    HU_ASSERT_TRUE(strstr(prompt, "sister") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "1") != NULL);

    alloc.free(alloc.ctx, prompt, out_len + 1);
    hu_persona_deinit(&alloc, &persona);
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
    HU_RUN_TEST(test_apply_route_with_fresh_activity);

    /* callback context builder */
    HU_RUN_TEST(test_build_callback_context_null_memory);
    HU_RUN_TEST(test_build_callback_context_null_msg);

    /* P6-1: channel overlay in proactive prompts */
    HU_RUN_TEST(test_p6_1_proactive_prompt_includes_channel_overlay);

    /* P6-2: relationship_type + dunbar_layer in proactive prompts */
    HU_RUN_TEST(test_p6_2_proactive_prompt_includes_relationship_type);
}
