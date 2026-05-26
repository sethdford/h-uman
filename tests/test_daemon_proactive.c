#include "human/agent.h"
#include "human/agent/governor.h"
#include "human/autoresponder.h"
#include "human/context/self_awareness.h"
#include "human/core/string.h"
#include "human/daemon_proactive.h"
#include "human/humanness.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdlib.h>
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

/* ── P6-5: shared absolute-rules block in proactive prompts ─────────── */
/*
 * The reactive path (src/agent/agent_stream.c:560-585) appends a
 * hard-override "ABSOLUTE RULES" block to the system prompt — it's
 * the highest-weight instruction the LLM sees ("You are HUMAN",
 * lowercase, no markdown, no em-dashes, etc). Without it the model
 * reverts to default-assistant register.
 *
 * P6-5 makes that block a shared function in persona.h, callable from
 * BOTH the reactive path AND the proactive path, so proactive sends
 * obey the same formatting rules as reactive replies.
 */
static void test_p6_5_proactive_prompt_includes_absolute_rules(void) {
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
    cp.contact_id = "user_a";
    cp.name = "Alice";
    cp.proactive_channel = "imessage:+1234567890";
    cp.proactive_checkin = true;

    size_t out_len = 0;
    char *prompt = hu_daemon_proactive_prompt_for_contact(&alloc, &agent, NULL, &cp, &out_len);
    HU_ASSERT_NOT_NULL(prompt);

    /* Substrings unique to the absolute-rules block. */
    HU_ASSERT_TRUE(strstr(prompt, "You are HUMAN") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "ZERO markdown") != NULL);

    alloc.free(alloc.ctx, prompt, out_len + 1);
    hu_persona_deinit(&alloc, &persona);
}

/* ── Sprint 41 — quiet-hour gate for proactive sends ─────────────────── */

/* Compose a daily DND window in local minutes. days_mask=0x7F = every day. */
static hu_autoresponder_config_t make_dnd_cfg(int16_t start_min, int16_t end_min,
                                              uint8_t days_mask) {
    hu_autoresponder_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.dnd_schedule[0].start_minute_of_day = start_min;
    cfg.dnd_schedule[0].end_minute_of_day = end_min;
    cfg.dnd_schedule[0].days_of_week_mask = days_mask;
    cfg.schedule_count = 1;
    return cfg;
}

/* Helper: pick a Unix timestamp anchored at a known local wall-clock
 * minute-of-day on a known day. Uses 2026-05-26 (Tuesday) as the date
 * floor, then adds the requested minute count. tz_offset=0 below means
 * we want now_unix to land at the LOCAL minute we name. */
static int64_t unix_at_local_minute(int hour, int minute) {
    /* 2026-05-26 00:00 UTC = 1779840000. Add hour*3600 + minute*60. */
    return (int64_t)1779840000 + (int64_t)hour * 3600 + (int64_t)minute * 60;
}

static void quiet_hours_skip_returns_false_when_cfg_is_null(void) {
    HU_ASSERT_FALSE(
        hu_daemon_proactive_should_skip_for_quiet_hours(NULL, unix_at_local_minute(3, 0), 0));
}

static void quiet_hours_skip_fires_inside_overnight_window(void) {
    /* DND 22:00 → 07:00, every day. 03:00 must fire. */
    hu_autoresponder_config_t cfg = make_dnd_cfg(22 * 60, 7 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(
        hu_daemon_proactive_should_skip_for_quiet_hours(&cfg, unix_at_local_minute(3, 0), 0));
}

static void quiet_hours_skip_holds_off_during_working_hours(void) {
    /* Same DND 22:00 → 07:00. 10:00 must NOT fire. */
    hu_autoresponder_config_t cfg = make_dnd_cfg(22 * 60, 7 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_FALSE(
        hu_daemon_proactive_should_skip_for_quiet_hours(&cfg, unix_at_local_minute(10, 0), 0));
}

static void quiet_hours_skip_fires_at_exact_start_of_window(void) {
    /* Boundary: 22:00 is the FIRST minute inside the window. */
    hu_autoresponder_config_t cfg = make_dnd_cfg(22 * 60, 7 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(
        hu_daemon_proactive_should_skip_for_quiet_hours(&cfg, unix_at_local_minute(22, 0), 0));
}

static void quiet_hours_skip_returns_false_when_cfg_disabled(void) {
    /* enabled=false → autoresponder code path returns false; we mirror. */
    hu_autoresponder_config_t cfg = make_dnd_cfg(22 * 60, 7 * 60, HU_DOW_MASK_DAILY);
    cfg.enabled = false;
    HU_ASSERT_FALSE(
        hu_daemon_proactive_should_skip_for_quiet_hours(&cfg, unix_at_local_minute(3, 0), 0));
}

/* ── Sprint 41 follow-up — daily-budget gate ──────────────────────────── */

static void budget_skip_returns_false_for_null_budget(void) {
    HU_ASSERT_FALSE(hu_daemon_proactive_should_skip_for_budget(NULL, 1000ULL));
}

/* governor.c::has_budget gates on BOTH daily_max AND weekly_max — both
 * must be > 0 for a budget to exist. relationship_multiplier MUST be
 * > 0 too (effective_daily_max multiplies by it); 1.0 = friend tier
 * per governor.h:12 docs. */
static hu_proactive_budget_config_t make_budget_cfg(uint8_t daily_max) {
    hu_proactive_budget_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.daily_max = daily_max;
    cfg.weekly_max = (uint8_t)(daily_max * 5);
    cfg.relationship_multiplier = 1.0;
    cfg.cool_off_hours = 0;
    return cfg;
}

static void budget_skip_returns_false_when_budget_remains(void) {
    hu_proactive_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    hu_proactive_budget_config_t cfg = make_budget_cfg(5);
    hu_governor_init(&cfg, &budget);
    HU_ASSERT_FALSE(hu_daemon_proactive_should_skip_for_budget(&budget, 1000ULL));
}

static void budget_skip_fires_when_daily_max_exhausted(void) {
    hu_proactive_budget_t budget;
    memset(&budget, 0, sizeof(budget));
    hu_proactive_budget_config_t cfg = make_budget_cfg(2);
    hu_governor_init(&cfg, &budget);
    /* Spend both budget slots. */
    HU_ASSERT_EQ(hu_governor_record_sent(&budget, 1000ULL), HU_OK);
    HU_ASSERT_EQ(hu_governor_record_sent(&budget, 1100ULL), HU_OK);
    /* Third send must be gated. */
    HU_ASSERT_TRUE(hu_daemon_proactive_should_skip_for_budget(&budget, 1200ULL));
}

/* P6-5 sanity: the shared helper produces the same key substrings. */
static void test_p6_5_absolute_rules_helper_emits_key_rules(void) {
    char buf[2048];
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_absolute_rules(NULL, buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "You are HUMAN") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ZERO markdown") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ABSOLUTE RULES") != NULL);
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

    /* P6-1: channel overlay in proactive prompts */
    HU_RUN_TEST(test_p6_1_proactive_prompt_includes_channel_overlay);

    /* P6-2: relationship_type + dunbar_layer in proactive prompts */
    HU_RUN_TEST(test_p6_2_proactive_prompt_includes_relationship_type);

    /* P6-5: shared absolute-rules block in proactive prompts */
    HU_RUN_TEST(test_p6_5_proactive_prompt_includes_absolute_rules);
    HU_RUN_TEST(test_p6_5_absolute_rules_helper_emits_key_rules);

    /* Sprint 41 — quiet-hour gate (Jordan incident 2026-05-26).
     * Pure-predicate truth-table coverage of the autoresponder DND
     * window check the daemon now consults before any proactive send. */
    HU_RUN_TEST(quiet_hours_skip_returns_false_when_cfg_is_null);
    HU_RUN_TEST(quiet_hours_skip_fires_inside_overnight_window);
    HU_RUN_TEST(quiet_hours_skip_holds_off_during_working_hours);
    HU_RUN_TEST(quiet_hours_skip_fires_at_exact_start_of_window);
    HU_RUN_TEST(quiet_hours_skip_returns_false_when_cfg_disabled);

    /* Sprint 41 follow-up — daily-budget gate parity with init_proposer. */
    HU_RUN_TEST(budget_skip_returns_false_for_null_budget);
    HU_RUN_TEST(budget_skip_returns_false_when_budget_remains);
    HU_RUN_TEST(budget_skip_fires_when_daily_max_exhausted);
}
