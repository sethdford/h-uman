#include "human/agent/app_config.h"
#include "human/config.h" /* full hu_config_t — the test constructs one on the stack */
#include "test_framework.h"
#include <string.h>

static void app_config_projects_core_fields(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Set representative fields the builder reads */
    cfg.default_provider = "gemini";
    cfg.default_model = "gemini-3.5-flash";
    cfg.temperature = 0.7;
    cfg.workspace_dir = "/tmp/test";
    cfg.memory_auto_save = true;
    cfg.security.autonomy_level = 1;
    cfg.agent.persona = "test-persona";
    cfg.agent.max_tool_iterations = 10;
    cfg.agent.max_history_messages = 50;
    cfg.agent.token_limit = 2048;
    cfg.agent.context_pressure_warn = 0.85f;
    cfg.agent.context_pressure_compact = 0.95f;
    cfg.agent.context_compact_target = 0.70f;
    cfg.agent.compact_context = true;
    cfg.agent.hula_enabled = true;
    cfg.agent.llm_compiler_enabled = false;

    hu_agent_app_config_t app = hu_agent_app_config_from(&cfg);

    /* Verify projections */
    HU_ASSERT_TRUE(app.default_provider != NULL);
    HU_ASSERT_EQ(strcmp(app.default_provider, "gemini"), 0);
    HU_ASSERT_EQ(app.default_provider_len, 6);

    HU_ASSERT_TRUE(app.default_model != NULL);
    HU_ASSERT_EQ(strcmp(app.default_model, "gemini-3.5-flash"), 0);
    HU_ASSERT_EQ(app.default_model_len, 16);

    HU_ASSERT_EQ(app.temperature, 0.7);
    HU_ASSERT_EQ(strcmp(app.workspace_dir, "/tmp/test"), 0);
    HU_ASSERT_EQ(app.auto_save, true);
    HU_ASSERT_EQ(app.autonomy_level, 1);
    HU_ASSERT_EQ(strcmp(app.persona, "test-persona"), 0);
    HU_ASSERT_EQ(app.max_tool_iterations, 10);
    HU_ASSERT_EQ(app.max_history_messages, 50);

    /* Context pressure fields */
    HU_ASSERT_EQ(app.token_limit, 2048);
    HU_ASSERT_TRUE(app.pressure_warn > 0.84f && app.pressure_warn < 0.86f);
    HU_ASSERT_TRUE(app.pressure_compact > 0.94f && app.pressure_compact < 0.96f);
    HU_ASSERT_TRUE(app.compact_target > 0.69f && app.compact_target < 0.71f);

    /* Feature gates */
    HU_ASSERT_EQ(app.compact_context, true);
    HU_ASSERT_EQ(app.hula_enabled, true);
    HU_ASSERT_EQ(app.llm_compiler_enabled, false);
}

static void app_config_handles_null_input(void) {
    /* NULL config yields zeroed projection (safe default) */
    hu_agent_app_config_t app = hu_agent_app_config_from(NULL);

    HU_ASSERT_EQ(app.default_provider, NULL);
    HU_ASSERT_EQ(app.default_provider_len, 0);
    HU_ASSERT_EQ(app.default_model, NULL);
    HU_ASSERT_EQ(app.temperature, 0.0);
    HU_ASSERT_EQ(app.auto_save, false);
    HU_ASSERT_EQ(app.token_limit, 0);
}

static void app_config_handles_null_strings(void) {
    /* Config with NULL fields yields safe zeroed projection */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* Leave strings NULL; builder should not crash */

    hu_agent_app_config_t app = hu_agent_app_config_from(&cfg);

    HU_ASSERT_EQ(app.default_provider, NULL);
    HU_ASSERT_EQ(app.default_provider_len, 0);
    HU_ASSERT_EQ(app.default_model, NULL);
    HU_ASSERT_EQ(app.default_model_len, 0);
    HU_ASSERT_EQ(app.workspace_dir, NULL);
    HU_ASSERT_EQ(app.workspace_dir_len, 0);
    HU_ASSERT_EQ(app.persona, NULL);
    HU_ASSERT_EQ(app.persona_len, 0);
}

void run_agent_app_config_tests(void) {
    HU_TEST_SUITE("agent_app_config");
    HU_RUN_TEST(app_config_projects_core_fields);
    HU_RUN_TEST(app_config_handles_null_input);
    HU_RUN_TEST(app_config_handles_null_strings);
}
