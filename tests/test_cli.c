#include "test_framework.h"
#include "seaclaw/cli_commands.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdio.h>
#include <stdlib.h>

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
}
