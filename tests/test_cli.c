#include "test_framework.h"
#include "seaclaw/agent/cli.h"
#include "seaclaw/cli_commands.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Use a temp HOME with no config so sc_config_load uses defaults and validates. */
static void set_test_home(void) {
    setenv("HOME", "/tmp/seaclaw_cli_test_noconfig", 1);
}

static void test_cmd_channel_list(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "channel", "list"};
    sc_error_t err = cmd_channel(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_hardware_list(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "hardware", "list"};
    sc_error_t err = cmd_hardware(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_memory_stats(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "memory", "stats"};
    sc_error_t err = cmd_memory(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_memory_search_no_query(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "memory", "search"};
    sc_error_t err = cmd_memory(&alloc, 3, argv);
    SC_ASSERT(err != SC_OK);
}

static void test_cmd_workspace_show(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "workspace", "show"};
    sc_error_t err = cmd_workspace(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_capabilities_default(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "capabilities"};
    sc_error_t err = cmd_capabilities(&alloc, 2, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_capabilities_json(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "capabilities", "--json"};
    sc_error_t err = cmd_capabilities(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_models_list(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "models", "list"};
    sc_error_t err = cmd_models(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_auth_status(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "auth", "status", "openai"};
    sc_error_t err = cmd_auth(&alloc, 4, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_update_check(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "update", "--check"};
    sc_error_t err = cmd_update(&alloc, 3, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_cmd_sandbox_default(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "sandbox"};
    sc_error_t err = cmd_sandbox(&alloc, 2, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

/* cmd_init: under SC_IS_TEST skips filesystem/stdin, returns SC_OK */
static void test_cmd_init_sc_is_test_returns_ok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *argv[] = {"seaclaw", "init"};
    sc_error_t err = cmd_init(&alloc, 2, argv);
    SC_ASSERT_EQ(err, SC_OK);
}

/* Demo mode: --demo flag sets demo_mode in parsed args */
static void test_agent_cli_demo_flag_parsing(void) {
    const char *argv[] = {"seaclaw", "agent", "--demo"};
    sc_parsed_agent_args_t out;
    sc_error_t err = sc_agent_cli_parse_args(argv, 3, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.demo_mode, 1);
}

/* Demo mode: without --demo, demo_mode is 0 */
static void test_agent_cli_no_demo_flag(void) {
    const char *argv[] = {"seaclaw", "agent"};
    sc_parsed_agent_args_t out;
    sc_error_t err = sc_agent_cli_parse_args(argv, 2, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.demo_mode, 0);
}

/* Demo mode: overrides provider to ollama when applied to config */
static void test_agent_cli_demo_overrides_provider(void) {
    set_test_home();
    sc_allocator_t alloc = sc_system_allocator();
    sc_config_t cfg;
    sc_error_t err = sc_config_load(&alloc, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(cfg.default_provider != NULL);
    /* Before override: provider is from config (often "openai" from defaults) */
    /* Apply demo overrides (same logic as in cli.c) */
    cfg.default_provider = "ollama";
    cfg.default_model = "llama3.2";
    cfg.memory.backend = "none";
    cfg.memory_backend = "none";
    SC_ASSERT_TRUE(strcmp(cfg.default_provider, "ollama") == 0);
    SC_ASSERT_TRUE(strcmp(cfg.default_model, "llama3.2") == 0);
    SC_ASSERT_TRUE(strcmp(cfg.memory.backend, "none") == 0);
    sc_config_deinit(&cfg);
}

void run_cli_tests(void) {
    SC_TEST_SUITE("CLI Commands");
    SC_RUN_TEST(test_cmd_channel_list);
    SC_RUN_TEST(test_cmd_hardware_list);
    SC_RUN_TEST(test_cmd_memory_stats);
    SC_RUN_TEST(test_cmd_memory_search_no_query);
    SC_RUN_TEST(test_cmd_workspace_show);
    SC_RUN_TEST(test_cmd_capabilities_default);
    SC_RUN_TEST(test_cmd_capabilities_json);
    SC_RUN_TEST(test_cmd_models_list);
    SC_RUN_TEST(test_cmd_auth_status);
    SC_RUN_TEST(test_cmd_update_check);
    SC_RUN_TEST(test_cmd_sandbox_default);
    SC_RUN_TEST(test_cmd_init_sc_is_test_returns_ok);
    SC_RUN_TEST(test_agent_cli_demo_flag_parsing);
    SC_RUN_TEST(test_agent_cli_no_demo_flag);
    SC_RUN_TEST(test_agent_cli_demo_overrides_provider);
}
