/* E2E agent loop: bootstrap → config parse → agent turn → teardown.
 * HU_IS_TEST: no real network, no process spawning. */
#include "human/bootstrap.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#endif
#include "human/persona.h"

static hu_config_t *make_config_with_arena(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    return cfg;
}

static void free_config(hu_config_t *cfg) {
    if (!cfg)
        return;
    hu_allocator_t backing = hu_system_allocator();
    if (cfg->arena)
        hu_arena_destroy(cfg->arena);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

static void e2e_agent_loop_bootstrap_teardown_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, false, false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx.alloc);
    HU_ASSERT_NOT_NULL(ctx.cfg);
    hu_app_teardown(&ctx);
}

static void e2e_agent_loop_teardown_null_is_safe(void) {
    hu_app_teardown(NULL);
}

static void e2e_agent_loop_teardown_zero_ctx_is_safe(void) {
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_app_teardown(&ctx);
}

static void e2e_config_parse_and_use(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *json = "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4\"}";
    hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg->default_provider);
    HU_ASSERT_STR_EQ(cfg->default_provider, "openai");
    HU_ASSERT_NOT_NULL(cfg->default_model);
    HU_ASSERT_STR_EQ(cfg->default_model, "gpt-4");
    free_config(cfg);
}

static void e2e_config_parse_empty_uses_defaults(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "{}", 2);
    HU_ASSERT_EQ(err, HU_OK);
    free_config(cfg);
}

static void e2e_bootstrap_minimal_no_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_error_t err = hu_app_bootstrap(&ctx, &alloc, NULL, false, false);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(ctx.tools_count > 0);
    HU_ASSERT_FALSE(ctx.agent_ok);
    hu_app_teardown(&ctx);
}

#if defined(HU_ENABLE_SQLITE)
static void e2e_agent_turn_with_persona_and_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_error_t err = hu_memory_store_for_contact(&mem, "mindy", 5, "pref:coffee", 11,
                                                 "Mindy loves oat milk lattes", 27, NULL, "", 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_memory_store_for_contact(&mem, "mindy", 5, "topic:job", 9,
                                      "Mindy is stressed about her new job at the startup", 51,
                                      NULL, "", 0);
    HU_ASSERT_EQ(err, HU_OK);

    const char *persona_json =
        "{"
        "\"name\": \"seth\","
        "\"core\": {"
        "\"identity\": \"A 30-something tech guy who uses casual language\","
        "\"traits\": [\"warm\", \"funny\", \"direct\"],"
        "\"vocabulary\": {\"preferred\": [\"dope\", \"yo\", \"lowkey\"]},"
        "\"communication_rules\": [\"Keep it casual\", \"Use lowercase mostly\"]"
        "},"
        "\"contacts\": {"
        "\"mindy\": {"
        "\"name\": \"Mindy\","
        "\"relationship\": \"close friend\","
        "\"relationship_stage\": \"friend\","
        "\"warmth_level\": \"high\","
        "\"greeting_style\": \"hey min!\","
        "\"interests\": [\"coffee\", \"startups\", \"hiking\"]"
        "}"
        "}"
        "}";
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    err = hu_persona_load_json(&alloc, persona_json, strlen(persona_json), &persona);
    HU_ASSERT_EQ(err, HU_OK);

    const hu_contact_profile_t *cp = hu_persona_find_contact(&persona, "mindy", 5);
    HU_ASSERT_NOT_NULL(cp);
    HU_ASSERT_STR_EQ(cp->name, "Mindy");

    char *prompt = NULL;
    size_t prompt_len = 0;
    err =
        hu_persona_build_prompt(&alloc, &persona, "imessage", 8, "coffee", 6, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "seth") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "casual") != NULL || strstr(prompt, "warm") != NULL ||
                   strstr(prompt, "tech") != NULL || strstr(prompt, "dope") != NULL ||
                   strstr(prompt, "Keep") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);

    char *contact_ctx = NULL;
    size_t contact_ctx_len = 0;
    err = hu_contact_profile_build_context(&alloc, cp, &contact_ctx, &contact_ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(contact_ctx);
    HU_ASSERT_TRUE(strstr(contact_ctx, "Mindy") != NULL);
    alloc.free(alloc.ctx, contact_ctx, contact_ctx_len + 1);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = hu_memory_recall_for_contact(&mem, &alloc, NULL, NULL, "mindy", 5, "coffee", 6, 5, "", 0,
                                       &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    bool found_coffee = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].content && strstr(entries[i].content, "oat milk"))
            found_coffee = true;
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    }
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    HU_ASSERT_TRUE(found_coffee);

    hu_persona_deinit(&alloc, &persona);
    mem.vtable->deinit(mem.ctx);
}

static void e2e_contact_scoped_memory_isolation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    const char *contact_a = "alice";
    const char *contact_b = "bob";
    size_t len_a = 5, len_b = 3;

    hu_error_t err = hu_memory_store_for_contact(&mem, contact_a, len_a, "pref", 4,
                                                 "Alice prefers espresso", 22, NULL, "", 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_memory_store_for_contact(&mem, contact_b, len_b, "pref", 4, "Bob prefers matcha tea",
                                      21, NULL, "", 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    err = hu_memory_recall_for_contact(&mem, &alloc, NULL, NULL, contact_a, len_a, "espresso", 8, 5,
                                       "", 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(entries[0].content && strstr(entries[0].content, "espresso") != NULL);
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(&alloc, &entries[i]);
    alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));

    entries = NULL;
    count = 0;
    err = hu_memory_recall_for_contact(&mem, &alloc, NULL, NULL, contact_a, len_a, "matcha", 6, 5,
                                       "", 0, &entries, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    if (entries) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(&alloc, &entries[i]);
        alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));
    }
    mem.vtable->deinit(mem.ctx);
}

static void e2e_persona_prompt_includes_temporal_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{"
                       "\"name\": \"seth\","
                       "\"identity\": \"test persona\","
                       "\"core_values\": [\"honesty\", \"curiosity\"],"
                       "\"relationships\": [{\"name\": \"Mom\", \"role\": \"mother\", \"notes\": "
                       "\"weekly calls\"}],"
                       "\"style_rules\": [\"never use exclamation marks\"],"
                       "\"anti_patterns\": [\"don't say 'as an AI'\"],"
                       "\"identity_reinforcement\": \"You are Seth, always.\""
                       "}";
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &persona);
    HU_ASSERT_EQ(err, HU_OK);

    char *prompt = NULL;
    size_t prompt_len = 0;
    err = hu_persona_build_prompt(&alloc, &persona, "imessage", 8, NULL, 0, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);

    HU_ASSERT_TRUE(strstr(prompt, "honesty") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Mom") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "exclamation") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "as an AI") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "You are Seth") != NULL);

    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    hu_persona_deinit(&alloc, &persona);
}
#endif

void run_e2e_agent_loop_tests(void) {
    HU_TEST_SUITE("E2E Agent Loop");

    HU_RUN_TEST(e2e_agent_loop_teardown_null_is_safe);
    HU_RUN_TEST(e2e_agent_loop_teardown_zero_ctx_is_safe);
    HU_RUN_TEST(e2e_config_parse_and_use);
    HU_RUN_TEST(e2e_config_parse_empty_uses_defaults);
    HU_RUN_TEST(e2e_agent_loop_bootstrap_teardown_safe);
    HU_RUN_TEST(e2e_bootstrap_minimal_no_agent);
#if defined(HU_ENABLE_SQLITE)
    HU_RUN_TEST(e2e_agent_turn_with_persona_and_contact);
    HU_RUN_TEST(e2e_contact_scoped_memory_isolation);
    HU_RUN_TEST(e2e_persona_prompt_includes_temporal_context);
#endif
}
