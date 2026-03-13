/* Tests for agent registry, config-driven creation, and skill unification. */
#include "human/agent/registry.h"
#include "human/agent/spawn.h"
#include "human/config_types.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/skillforge.h"
#include "human/tools/skill_run.h"
#include "human/tool.h"
#include "test_framework.h"
#include <string.h>

/* ─── agent registry ──────────────────────────────────────────────────────── */

static void test_registry_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_error_t err = hu_agent_registry_create(&alloc, &reg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(reg.count, 0u);
    hu_agent_registry_destroy(&reg);
}

static void test_registry_register_and_get(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_named_agent_config_t cfg = {
        .name = "researcher",
        .provider = "anthropic",
        .model = "claude-sonnet",
        .persona = "analyst",
        .role = "builder",
        .description = "Research agent",
        .capabilities = "research,analysis",
        .autonomy_level = 3,
        .temperature = 0.7,
        .budget_usd = 1.0,
        .max_iterations = 20,
    };

    hu_error_t err = hu_agent_registry_register(&reg, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(reg.count, 1u);

    const hu_named_agent_config_t *found = hu_agent_registry_get(&reg, "researcher");
    HU_ASSERT_NOT_NULL(found);
    HU_ASSERT_STR_EQ(found->name, "researcher");
    HU_ASSERT_STR_EQ(found->provider, "anthropic");
    HU_ASSERT_STR_EQ(found->model, "claude-sonnet");
    HU_ASSERT_STR_EQ(found->persona, "analyst");
    HU_ASSERT_STR_EQ(found->role, "builder");
    HU_ASSERT_STR_EQ(found->description, "Research agent");
    HU_ASSERT_STR_EQ(found->capabilities, "research,analysis");
    HU_ASSERT_EQ(found->autonomy_level, 3u);

    hu_agent_registry_destroy(&reg);
}

static void test_registry_not_found_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    const hu_named_agent_config_t *found = hu_agent_registry_get(&reg, "nonexistent");
    HU_ASSERT_EQ(found, NULL);

    hu_agent_registry_destroy(&reg);
}

static void test_registry_reject_duplicate_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_named_agent_config_t cfg = {.name = "helper", .provider = "openai", .model = "gpt-4"};
    hu_error_t err = hu_agent_registry_register(&reg, &cfg);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_agent_registry_register(&reg, &cfg);
    HU_ASSERT_EQ(err, HU_ERR_ALREADY_EXISTS);
    HU_ASSERT_EQ(reg.count, 1u);

    hu_agent_registry_destroy(&reg);
}

static void test_registry_find_by_capability(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_named_agent_config_t a1 = {
        .name = "researcher", .capabilities = "research,analysis"};
    hu_named_agent_config_t a2 = {
        .name = "coder", .capabilities = "coding,testing"};
    hu_named_agent_config_t a3 = {
        .name = "analyst", .capabilities = "analysis,reporting"};

    hu_agent_registry_register(&reg, &a1);
    hu_agent_registry_register(&reg, &a2);
    hu_agent_registry_register(&reg, &a3);

    const hu_named_agent_config_t *results[8];
    size_t count = 0;

    hu_error_t err =
        hu_agent_registry_find_by_capability(&reg, "analysis", results, 8, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(results[0]->name, "researcher");
    HU_ASSERT_STR_EQ(results[1]->name, "analyst");

    count = 0;
    err = hu_agent_registry_find_by_capability(&reg, "coding", results, 8, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(results[0]->name, "coder");

    count = 0;
    err = hu_agent_registry_find_by_capability(&reg, "nonexistent", results, 8, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    hu_agent_registry_destroy(&reg);
}

static void test_registry_get_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_named_agent_config_t a1 = {.name = "first"};
    hu_named_agent_config_t a2 = {.name = "second", .is_default = true};

    hu_agent_registry_register(&reg, &a1);
    hu_agent_registry_register(&reg, &a2);

    const hu_named_agent_config_t *def = hu_agent_registry_get_default(&reg);
    HU_ASSERT_NOT_NULL(def);
    HU_ASSERT_STR_EQ(def->name, "second");

    hu_agent_registry_destroy(&reg);
}

static void test_registry_get_default_falls_back_to_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_named_agent_config_t a1 = {.name = "first"};
    hu_named_agent_config_t a2 = {.name = "second"};

    hu_agent_registry_register(&reg, &a1);
    hu_agent_registry_register(&reg, &a2);

    const hu_named_agent_config_t *def = hu_agent_registry_get_default(&reg);
    HU_ASSERT_NOT_NULL(def);
    HU_ASSERT_STR_EQ(def->name, "first");

    hu_agent_registry_destroy(&reg);
}

static void test_registry_empty_returns_null_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    const hu_named_agent_config_t *def = hu_agent_registry_get_default(&reg);
    HU_ASSERT_EQ(def, NULL);

    hu_agent_registry_destroy(&reg);
}

/* ─── JSON parsing ────────────────────────────────────────────────────────── */

static void test_registry_parse_json_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"name\":\"test-agent\",\"provider\":\"openai\","
        "\"model\":\"gpt-4\",\"persona\":\"helper\","
        "\"role\":\"builder\",\"autonomy_level\":2,"
        "\"temperature\":0.5,\"budget_usd\":2.0,"
        "\"max_iterations\":10,\"is_default\":true,"
        "\"description\":\"A test agent\","
        "\"capabilities\":\"testing,coding\","
        "\"enabled_tools\":[\"shell\",\"file_read\"],"
        "\"enabled_skills\":[\"deep_research\"]}";

    hu_named_agent_config_t cfg = {0};
    hu_error_t err = hu_agent_registry_parse_json(&alloc, json, strlen(json), &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.name, "test-agent");
    HU_ASSERT_STR_EQ(cfg.provider, "openai");
    HU_ASSERT_STR_EQ(cfg.model, "gpt-4");
    HU_ASSERT_STR_EQ(cfg.persona, "helper");
    HU_ASSERT_STR_EQ(cfg.role, "builder");
    HU_ASSERT_EQ(cfg.autonomy_level, 2u);
    HU_ASSERT_TRUE(cfg.temperature > 0.4 && cfg.temperature < 0.6);
    HU_ASSERT_TRUE(cfg.budget_usd > 1.9 && cfg.budget_usd < 2.1);
    HU_ASSERT_EQ(cfg.max_iterations, 10u);
    HU_ASSERT_TRUE(cfg.is_default);
    HU_ASSERT_STR_EQ(cfg.description, "A test agent");
    HU_ASSERT_STR_EQ(cfg.capabilities, "testing,coding");
    HU_ASSERT_EQ(cfg.enabled_tools_count, 2u);
    HU_ASSERT_STR_EQ(cfg.enabled_tools[0], "shell");
    HU_ASSERT_STR_EQ(cfg.enabled_tools[1], "file_read");
    HU_ASSERT_EQ(cfg.enabled_skills_count, 1u);
    HU_ASSERT_STR_EQ(cfg.enabled_skills[0], "deep_research");

    hu_named_agent_config_free(&alloc, &cfg);
}

static void test_registry_parse_json_minimal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"name\":\"minimal\"}";

    hu_named_agent_config_t cfg = {0};
    hu_error_t err = hu_agent_registry_parse_json(&alloc, json, strlen(json), &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.name, "minimal");
    HU_ASSERT_EQ(cfg.provider, NULL);
    HU_ASSERT_EQ(cfg.model, NULL);
    HU_ASSERT_EQ(cfg.enabled_tools_count, 0u);
    HU_ASSERT_EQ(cfg.enabled_skills_count, 0u);

    hu_named_agent_config_free(&alloc, &cfg);
}

static void test_registry_parse_json_missing_name_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"provider\":\"openai\"}";

    hu_named_agent_config_t cfg = {0};
    hu_error_t err = hu_agent_registry_parse_json(&alloc, json, strlen(json), &cfg);
    HU_ASSERT_EQ(err, HU_ERR_JSON_PARSE);
}

static void test_registry_parse_json_invalid_json_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_named_agent_config_t cfg = {0};
    hu_error_t err = hu_agent_registry_parse_json(&alloc, "not json", 8, &cfg);
    HU_ASSERT_TRUE(err != HU_OK);
}

/* ─── named agent config free ────────────────────────────────────────────── */

static void test_named_agent_config_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_named_agent_config_free(&alloc, NULL);
    hu_named_agent_config_free(NULL, NULL);
}

/* ─── spawn config from named ────────────────────────────────────────────── */

static void test_spawn_config_from_named(void) {
    hu_named_agent_config_t cfg = {
        .name = "worker",
        .provider = "anthropic",
        .model = "claude-sonnet",
        .persona = "helper",
        .system_prompt = "You are helpful.",
        .temperature = 0.8,
        .budget_usd = 5.0,
        .max_iterations = 30,
    };

    hu_spawn_config_t spawn = {0};
    hu_spawn_config_from_named(&spawn, &cfg);

    HU_ASSERT_STR_EQ(spawn.provider, "anthropic");
    HU_ASSERT_EQ(spawn.provider_len, 9u);
    HU_ASSERT_STR_EQ(spawn.model, "claude-sonnet");
    HU_ASSERT_EQ(spawn.model_len, 13u);
    HU_ASSERT_STR_EQ(spawn.persona_name, "helper");
    HU_ASSERT_EQ(spawn.persona_name_len, 6u);
    HU_ASSERT_STR_EQ(spawn.system_prompt, "You are helpful.");
    HU_ASSERT_TRUE(spawn.temperature > 0.7 && spawn.temperature < 0.9);
    HU_ASSERT_TRUE(spawn.budget_usd > 4.9 && spawn.budget_usd < 5.1);
    HU_ASSERT_EQ(spawn.max_iterations, 30u);
    HU_ASSERT_EQ(spawn.mode, HU_SPAWN_ONE_SHOT);
}

static void test_spawn_config_from_named_null_safe(void) {
    hu_spawn_config_t spawn = {0};
    hu_spawn_config_from_named(&spawn, NULL);
    hu_spawn_config_from_named(NULL, NULL);
}

/* ─── discover (test mode: no-op) ────────────────────────────────────────── */

static void test_registry_discover_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_error_t err = hu_agent_registry_discover(&reg, "/tmp/nonexistent");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(reg.count, 0u);

    hu_agent_registry_destroy(&reg);
}

static void test_registry_reload_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    hu_error_t err = hu_agent_registry_reload(&reg);
    HU_ASSERT_EQ(err, HU_OK);

    hu_agent_registry_destroy(&reg);
}

/* ─── skill_run tool ──────────────────────────────────────────────────────── */

static void test_skill_run_finds_skill(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");

    hu_tool_t tool;
    hu_error_t err = hu_skill_run_create(&alloc, &tool, &sf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "skill_run");

    /* Build args: {"skill": "test-skill"} */
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "skill", hu_json_string_new(&alloc, "test-skill", 10));

    hu_tool_result_t result = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);
    HU_ASSERT_NOT_NULL(result.output);
    HU_ASSERT_TRUE(strstr(result.output, "test-skill") != NULL);
    HU_ASSERT_TRUE(strstr(result.output, "echo test") != NULL);

    if (result.output_owned)
        alloc.free(alloc.ctx, (void *)result.output, result.output_len + 1);
    hu_json_free(&alloc, args);
    tool.vtable->deinit(tool.ctx, &alloc);
    hu_skillforge_destroy(&sf);
}

static void test_skill_run_missing_skill_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");

    hu_tool_t tool;
    hu_skill_run_create(&alloc, &tool, &sf);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "skill",
                       hu_json_string_new(&alloc, "nonexistent", 11));

    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);

    if (result.error_msg_owned)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    hu_json_free(&alloc, args);
    tool.vtable->deinit(tool.ctx, &alloc);
    hu_skillforge_destroy(&sf);
}

static void test_skill_run_disabled_skill_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");

    hu_tool_t tool;
    hu_skill_run_create(&alloc, &tool, &sf);

    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "skill",
                       hu_json_string_new(&alloc, "another-skill", 13));

    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(result.success);
    HU_ASSERT_TRUE(strstr(result.error_msg, "disabled") != NULL);

    if (result.error_msg_owned)
        alloc.free(alloc.ctx, (void *)result.error_msg, result.error_msg_len + 1);
    hu_json_free(&alloc, args);
    tool.vtable->deinit(tool.ctx, &alloc);
    hu_skillforge_destroy(&sf);
}

/* ─── skillforge command field ────────────────────────────────────────────── */

static void test_skillforge_has_command_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, "/tmp");

    hu_skill_t *skill = hu_skillforge_get_skill(&sf, "test-skill");
    HU_ASSERT_NOT_NULL(skill);
    HU_ASSERT_NOT_NULL(skill->command);
    HU_ASSERT_STR_EQ(skill->command, "echo test");

    hu_skill_t *no_cmd = hu_skillforge_get_skill(&sf, "another-skill");
    HU_ASSERT_NOT_NULL(no_cmd);
    HU_ASSERT_EQ(no_cmd->command, NULL);

    hu_skillforge_destroy(&sf);
}

/* ─── registry with tools ────────────────────────────────────────────────── */

static void test_registry_register_with_tools_and_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_registry_t reg;
    hu_agent_registry_create(&alloc, &reg);

    const char *tools[] = {"shell", "file_read", "web_fetch"};
    const char *skills[] = {"deep_research"};

    hu_named_agent_config_t cfg = {
        .name = "full-agent",
        .provider = "openai",
        .model = "gpt-4",
        .enabled_tools = tools,
        .enabled_tools_count = 3,
        .enabled_skills = skills,
        .enabled_skills_count = 1,
    };

    hu_error_t err = hu_agent_registry_register(&reg, &cfg);
    HU_ASSERT_EQ(err, HU_OK);

    const hu_named_agent_config_t *found = hu_agent_registry_get(&reg, "full-agent");
    HU_ASSERT_NOT_NULL(found);
    HU_ASSERT_EQ(found->enabled_tools_count, 3u);
    HU_ASSERT_STR_EQ(found->enabled_tools[0], "shell");
    HU_ASSERT_STR_EQ(found->enabled_tools[1], "file_read");
    HU_ASSERT_STR_EQ(found->enabled_tools[2], "web_fetch");
    HU_ASSERT_EQ(found->enabled_skills_count, 1u);
    HU_ASSERT_STR_EQ(found->enabled_skills[0], "deep_research");

    hu_agent_registry_destroy(&reg);
}

/* ─── suite runner ────────────────────────────────────────────────────────── */

void run_agent_registry_tests(void) {
    HU_TEST_SUITE("agent_registry");

    HU_RUN_TEST(test_registry_create_destroy);
    HU_RUN_TEST(test_registry_register_and_get);
    HU_RUN_TEST(test_registry_not_found_returns_null);
    HU_RUN_TEST(test_registry_reject_duplicate_name);
    HU_RUN_TEST(test_registry_find_by_capability);
    HU_RUN_TEST(test_registry_get_default);
    HU_RUN_TEST(test_registry_get_default_falls_back_to_first);
    HU_RUN_TEST(test_registry_empty_returns_null_default);
    HU_RUN_TEST(test_registry_parse_json_basic);
    HU_RUN_TEST(test_registry_parse_json_minimal);
    HU_RUN_TEST(test_registry_parse_json_missing_name_fails);
    HU_RUN_TEST(test_registry_parse_json_invalid_json_fails);
    HU_RUN_TEST(test_named_agent_config_free_null_safe);
    HU_RUN_TEST(test_spawn_config_from_named);
    HU_RUN_TEST(test_spawn_config_from_named_null_safe);
    HU_RUN_TEST(test_registry_discover_test_mode);
    HU_RUN_TEST(test_registry_reload_empty);
    HU_RUN_TEST(test_skill_run_finds_skill);
    HU_RUN_TEST(test_skill_run_missing_skill_fails);
    HU_RUN_TEST(test_skill_run_disabled_skill_fails);
    HU_RUN_TEST(test_skillforge_has_command_field);
    HU_RUN_TEST(test_registry_register_with_tools_and_skills);
}
