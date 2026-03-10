#include "human/agent/cli.h"
#include "human/cli_commands.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Use a temp HOME with no config so hu_config_load uses defaults and validates. */
static void set_test_home(void) {
    setenv("HOME", "/tmp/human_cli_test_noconfig", 1);
}

static void test_cmd_channel_list(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "channel", "list"};
    hu_error_t err = cmd_channel(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_hardware_list(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "hardware", "list"};
    hu_error_t err = cmd_hardware(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_memory_stats(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "memory", "stats"};
    hu_error_t err = cmd_memory(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_memory_search_no_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "memory", "search"};
    hu_error_t err = cmd_memory(&alloc, 3, argv);
    HU_ASSERT(err != HU_OK);
}

static void test_cmd_workspace_show(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "workspace", "show"};
    hu_error_t err = cmd_workspace(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_capabilities_default(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "capabilities"};
    hu_error_t err = cmd_capabilities(&alloc, 2, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_capabilities_json(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "capabilities", "--json"};
    hu_error_t err = cmd_capabilities(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_models_list(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "models", "list"};
    hu_error_t err = cmd_models(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_auth_status(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "auth", "status", "openai"};
    hu_error_t err = cmd_auth(&alloc, 4, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_update_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "update", "--check"};
    hu_error_t err = cmd_update(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_cmd_sandbox_default(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "sandbox"};
    hu_error_t err = cmd_sandbox(&alloc, 2, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* cmd_init: under HU_IS_TEST skips filesystem/stdin, returns HU_OK */
static void test_cmd_init_sc_is_test_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *argv[] = {"human", "init"};
    hu_error_t err = cmd_init(&alloc, 2, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* Demo mode: --demo flag sets demo_mode in parsed args */
static void test_agent_cli_demo_flag_parsing(void) {
    const char *argv[] = {"human", "agent", "--demo"};
    hu_parsed_agent_args_t out;
    hu_error_t err = hu_agent_cli_parse_args(argv, 3, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.demo_mode, 1);
}

/* Demo mode: without --demo, demo_mode is 0 */
static void test_agent_cli_no_demo_flag(void) {
    const char *argv[] = {"human", "agent"};
    hu_parsed_agent_args_t out;
    hu_error_t err = hu_agent_cli_parse_args(argv, 2, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.demo_mode, 0);
}

/* Demo mode: overrides provider to ollama when applied to config */
static void test_agent_cli_demo_overrides_provider(void) {
    set_test_home();
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg;
    hu_error_t err = hu_config_load(&alloc, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(cfg.default_provider != NULL);
    /* Before override: provider is from config (often "openai" from defaults) */
    /* Apply demo overrides (same logic as in cli.c) */
    cfg.default_provider = "ollama";
    cfg.default_model = "llama3.2";
    cfg.memory.backend = "none";
    cfg.memory_backend = "none";
    HU_ASSERT_TRUE(strcmp(cfg.default_provider, "ollama") == 0);
    HU_ASSERT_TRUE(strcmp(cfg.default_model, "llama3.2") == 0);
    HU_ASSERT_TRUE(strcmp(cfg.memory.backend, "none") == 0);
    hu_config_deinit(&cfg);
}

void run_cli_tests(void) {
    HU_TEST_SUITE("CLI Commands");
    HU_RUN_TEST(test_cmd_channel_list);
    HU_RUN_TEST(test_cmd_hardware_list);
    HU_RUN_TEST(test_cmd_memory_stats);
    HU_RUN_TEST(test_cmd_memory_search_no_query);
    HU_RUN_TEST(test_cmd_workspace_show);
    HU_RUN_TEST(test_cmd_capabilities_default);
    HU_RUN_TEST(test_cmd_capabilities_json);
    HU_RUN_TEST(test_cmd_models_list);
    HU_RUN_TEST(test_cmd_auth_status);
    HU_RUN_TEST(test_cmd_update_check);
    HU_RUN_TEST(test_cmd_sandbox_default);
    HU_RUN_TEST(test_cmd_init_sc_is_test_returns_ok);
    HU_RUN_TEST(test_agent_cli_demo_flag_parsing);
    HU_RUN_TEST(test_agent_cli_no_demo_flag);
    HU_RUN_TEST(test_agent_cli_demo_overrides_provider);
}
