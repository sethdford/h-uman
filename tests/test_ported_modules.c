/* Tests for newly ported modules (capabilities, channel_catalog, config_mutator, update, etc.) */
#include "test_framework.h"
#include "seaclaw/capabilities.h"
#include "seaclaw/channel_catalog.h"
#include "seaclaw/channel_adapters.h"
#include "seaclaw/config_mutator.h"
#include "seaclaw/doctor.h"
#include "seaclaw/agent/commands.h"
#include "seaclaw/security.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/update.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/config.h"
#include <string.h>

static void test_channel_catalog_all(void) {
    size_t n = 0;
    const sc_channel_meta_t *m = sc_channel_catalog_all(&n);
    SC_ASSERT_NOT_NULL(m);
    SC_ASSERT(n >= 1);  /* at least cli when SC_HAS_CLI */
}

static void test_channel_catalog_find_by_key(void) {
    /* cli is always in catalog when built with CLI */
    const sc_channel_meta_t *t = sc_channel_catalog_find_by_key("cli");
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_STR_EQ(t->key, "cli");
}

static void test_channel_catalog_parse_peer_kind(void) {
    int r = sc_channel_adapters_parse_peer_kind("direct", 6);
    SC_ASSERT_EQ(r, (int)SC_CHAT_DIRECT);
    r = sc_channel_adapters_parse_peer_kind("group", 5);
    SC_ASSERT_EQ(r, (int)SC_CHAT_GROUP);
    r = sc_channel_adapters_parse_peer_kind("invalid", 7);
    SC_ASSERT_EQ(r, -1);
}

static void test_config_mutator_path_requires_restart(void) {
    SC_ASSERT_TRUE(sc_config_mutator_path_requires_restart("channels.telegram"));
    SC_ASSERT_TRUE(sc_config_mutator_path_requires_restart("memory.backend"));
    SC_ASSERT_FALSE(sc_config_mutator_path_requires_restart("default_temperature"));
}

static void test_doctor_parse_df(void) {
    const char *df = "Filesystem 1M-blocks Used Available Use% Mounted on\n/dev/sda1 1000 500 500 50% /\n";
    unsigned long mb = sc_doctor_parse_df_available_mb(df, strlen(df));
    SC_ASSERT_EQ(mb, 500ul);
}

static void test_agent_commands_parse(void) {
    const sc_slash_cmd_t *c = sc_agent_commands_parse("/new", 4);
    SC_ASSERT_NOT_NULL(c);
    SC_ASSERT_STR_EQ(c->name, "new");
}

static void test_agent_commands_bare_reset_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *prompt = NULL;
    sc_error_t err = sc_agent_commands_bare_session_reset_prompt(&alloc, "/new", 4, &prompt);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT(strstr(prompt, "Session Startup") != NULL);
    alloc.free(alloc.ctx, prompt, strlen(prompt) + 1);
}

static void test_rate_tracker(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_rate_tracker_t *t = sc_rate_tracker_create(&alloc, 3);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_FALSE(sc_rate_tracker_is_limited(t));
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_record_action(t));
    SC_ASSERT_FALSE(sc_rate_tracker_record_action(t));
    SC_ASSERT_TRUE(sc_rate_tracker_is_limited(t));
    sc_rate_tracker_destroy(t);
}

static void test_sandbox_create_noop(void) {
    sc_sandbox_t sb = sc_sandbox_create_noop();
    SC_ASSERT_TRUE(sc_sandbox_is_available(&sb));
    SC_ASSERT_STR_EQ(sc_sandbox_name(&sb), "none");
}

static void test_capabilities_manifest(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *json = NULL;
    sc_error_t err = sc_capabilities_build_manifest_json(&alloc, NULL, NULL, 0, &json);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(json);
    SC_ASSERT(strstr(json, "\"channels\"") != NULL);
    SC_ASSERT(strstr(json, "\"memory_engines\"") != NULL);
    alloc.free(alloc.ctx, json, strlen(json) + 1);
}

static void test_config_mutator_mutate(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mutation_result_t res = {0};
    sc_error_t err = sc_config_mutator_mutate(&alloc, SC_MUTATION_SET,
        "default_temperature", "0.5",
        (sc_mutation_options_t){.apply = false}, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(res.path);
    SC_ASSERT_STR_EQ(res.path, "default_temperature");
    sc_config_mutator_free_result(&alloc, &res);
}

static void test_config_mutator_mutate_denied_path(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mutation_result_t res = {0};
    sc_error_t err = sc_config_mutator_mutate(&alloc, SC_MUTATION_SET,
        "identity.format", "compact",
        (sc_mutation_options_t){.apply = false}, &res);
    SC_ASSERT_EQ(err, SC_ERR_PERMISSION_DENIED);
}

static void test_config_mutator_get_path_denied(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *json = NULL;
    sc_error_t err = sc_config_mutator_get_path_value_json(&alloc, "identity.format", &json);
    SC_ASSERT_EQ(err, SC_ERR_PERMISSION_DENIED);
    SC_ASSERT_NULL(json);
}

static void test_config_mutator_mutate_unset(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mutation_result_t res = {0};
    sc_error_t err = sc_config_mutator_mutate(&alloc, SC_MUTATION_UNSET,
        "memory.backend", NULL,
        (sc_mutation_options_t){.apply = false}, &res);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(res.path, "memory.backend");
    SC_ASSERT_TRUE(res.requires_restart);
    sc_config_mutator_free_result(&alloc, &res);
}

static void test_update_check_mock(void) {
    /* In SC_IS_TEST mode, returns mock version without network calls */
    char buf[64];
    sc_error_t err = sc_update_check(buf, sizeof(buf));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strlen(buf) > 0);
    SC_ASSERT_TRUE(strstr(buf, "mock") != NULL || strstr(buf, "99") != NULL);
}

static void test_update_apply_mock(void) {
    /* In SC_IS_TEST mode, returns SC_OK without applying */
    sc_error_t err = sc_update_apply();
    SC_ASSERT_EQ(err, SC_OK);
}

void run_ported_modules_tests(void) {
    SC_TEST_SUITE("Ported Modules");
    SC_RUN_TEST(test_channel_catalog_all);
    SC_RUN_TEST(test_channel_catalog_find_by_key);
    SC_RUN_TEST(test_channel_catalog_parse_peer_kind);
    SC_RUN_TEST(test_config_mutator_path_requires_restart);
    SC_RUN_TEST(test_config_mutator_get_path_denied);
    SC_RUN_TEST(test_config_mutator_mutate_denied_path);
    SC_RUN_TEST(test_config_mutator_mutate_unset);
    SC_RUN_TEST(test_doctor_parse_df);
    SC_RUN_TEST(test_agent_commands_parse);
    SC_RUN_TEST(test_agent_commands_bare_reset_prompt);
    SC_RUN_TEST(test_rate_tracker);
    SC_RUN_TEST(test_sandbox_create_noop);
    SC_RUN_TEST(test_capabilities_manifest);
    SC_RUN_TEST(test_config_mutator_mutate);
    SC_RUN_TEST(test_update_check_mock);
    SC_RUN_TEST(test_update_apply_mock);
}
