/* Tests for newly ported modules (capabilities, channel_catalog, config_mutator, update, etc.) */
#include "human/agent/commands.h"
#include "human/capabilities.h"
#include "human/channel_adapters.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/config_mutator.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/doctor.h"
#include "human/security.h"
#include "human/security/sandbox.h"
#include "human/service.h"
#include "human/update.h"
#include "test_framework.h"
#include <string.h>

static void test_channel_catalog_all(void) {
    size_t n = 0;
    const hu_channel_meta_t *m = hu_channel_catalog_all(&n);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT(n >= 1); /* at least cli when HU_HAS_CLI */
}

static void test_channel_catalog_find_by_key(void) {
    /* cli is always in catalog when built with CLI */
    const hu_channel_meta_t *t = hu_channel_catalog_find_by_key("cli");
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_STR_EQ(t->key, "cli");
}

static void test_channel_catalog_parse_peer_kind(void) {
    int r = hu_channel_adapters_parse_peer_kind("direct", 6);
    HU_ASSERT_EQ(r, (int)HU_CHAT_DIRECT);
    r = hu_channel_adapters_parse_peer_kind("group", 5);
    HU_ASSERT_EQ(r, (int)HU_CHAT_GROUP);
    r = hu_channel_adapters_parse_peer_kind("invalid", 7);
    HU_ASSERT_EQ(r, -1);
}

static void test_config_mutator_path_requires_restart(void) {
    HU_ASSERT_TRUE(hu_config_mutator_path_requires_restart("channels.telegram"));
    HU_ASSERT_TRUE(hu_config_mutator_path_requires_restart("memory.backend"));
    HU_ASSERT_FALSE(hu_config_mutator_path_requires_restart("default_temperature"));
}

static void test_doctor_parse_df(void) {
    const char *df =
        "Filesystem 1M-blocks Used Available Use% Mounted on\n/dev/sda1 1000 500 500 50% /\n";
    unsigned long mb = hu_doctor_parse_df_available_mb(df, strlen(df));
    HU_ASSERT_EQ(mb, 500ul);
}

static void test_doctor_truncate_null_alloc(void) {
    char *out = (char *)0x1;
    hu_error_t err = hu_doctor_truncate_for_display(NULL, "hello", 5, 10, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_truncate_null_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = (char *)0x1;
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, NULL, 0, 10, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_doctor_truncate_zero_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    const char *s = "hello";
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, s, 0, 10, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hello");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_doctor_truncate_normal_truncation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    const char *s = "hello world";
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, s, 11, 5, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strlen(out), 5u);
    HU_ASSERT_TRUE(strncmp(out, "hello", 5) == 0);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_doctor_truncate_shorter_than_max(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    const char *s = "hi";
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, s, 2, 10, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hi");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_doctor_truncate_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, "hello", 5, 10, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_null_alloc(void) {
    hu_config_t cfg = {0};
    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(NULL, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_null_cfg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, NULL, &items, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "openai";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_valid_with_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "openai";
    char key[] = "telegram";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    cfg.channels.channel_config_keys[0] = key;
    cfg.channels.channel_config_counts[0] = 1;
    cfg.channels.channel_config_len = 1;

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(count > 0);

    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc.free(alloc.ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc.free(alloc.ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    alloc.free(alloc.ctx, items, sizeof(hu_diag_item_t) * 16);
}

static void test_agent_commands_parse(void) {
    const hu_slash_cmd_t *c = hu_agent_commands_parse("/new", 4);
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_STR_EQ(c->name, "new");
}

static void test_agent_commands_bare_reset_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    hu_error_t err = hu_agent_commands_bare_session_reset_prompt(&alloc, "/new", 4, &prompt);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT(strstr(prompt, "Session Startup") != NULL);
    alloc.free(alloc.ctx, prompt, strlen(prompt) + 1);
}

static void test_rate_tracker(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rate_tracker_t *t = hu_rate_tracker_create(&alloc, 3);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_FALSE(hu_rate_tracker_is_limited(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_FALSE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_is_limited(t));
    hu_rate_tracker_destroy(t);
}

static void test_sandbox_create_noop(void) {
    hu_sandbox_t sb = hu_sandbox_create_noop();
    HU_ASSERT_TRUE(hu_sandbox_is_available(&sb));
    HU_ASSERT_STR_EQ(hu_sandbox_name(&sb), "none");
}

static void test_capabilities_manifest(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    hu_error_t err = hu_capabilities_build_manifest_json(&alloc, NULL, NULL, 0, &json);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT(strstr(json, "\"channels\"") != NULL);
    HU_ASSERT(strstr(json, "\"memory_engines\"") != NULL);
    alloc.free(alloc.ctx, json, strlen(json) + 1);
}

static void test_config_mutator_mutate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_error_t err = hu_config_mutator_mutate(&alloc, HU_MUTATION_SET, "default_temperature", "0.5",
                                              (hu_mutation_options_t){.apply = false}, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(res.path);
    HU_ASSERT_STR_EQ(res.path, "default_temperature");
    hu_config_mutator_free_result(&alloc, &res);
}

static void test_config_mutator_mutate_denied_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_error_t err = hu_config_mutator_mutate(&alloc, HU_MUTATION_SET, "identity.format", "compact",
                                              (hu_mutation_options_t){.apply = false}, &res);
    HU_ASSERT_EQ(err, HU_ERR_PERMISSION_DENIED);
}

static void test_config_mutator_get_path_denied(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    hu_error_t err = hu_config_mutator_get_path_value_json(&alloc, "identity.format", &json);
    HU_ASSERT_EQ(err, HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(json);
}

static void test_config_mutator_mutate_unset(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_error_t err = hu_config_mutator_mutate(&alloc, HU_MUTATION_UNSET, "memory.backend", NULL,
                                              (hu_mutation_options_t){.apply = false}, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(res.path, "memory.backend");
    HU_ASSERT_TRUE(res.requires_restart);
    hu_config_mutator_free_result(&alloc, &res);
}

static void test_update_check_mock(void) {
    /* In HU_IS_TEST mode, returns mock version without network calls */
    char buf[64];
    hu_error_t err = hu_update_check(buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strlen(buf) > 0);
    HU_ASSERT_TRUE(strstr(buf, "mock") != NULL || strstr(buf, "99") != NULL);
}

static void test_update_apply_mock(void) {
    /* In HU_IS_TEST mode, returns HU_OK without applying */
    hu_error_t err = hu_update_apply();
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_service_start_stop(void) {
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_service_status());
    hu_service_stop();
    HU_ASSERT_FALSE(hu_service_status());
}

static void test_service_configure_null(void) {
    hu_service_configure(NULL, NULL);
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    hu_service_stop();
}

static void test_service_double_start(void) {
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    hu_service_stop();
}

static void test_service_configure_with_ctx(void) {
    hu_channel_loop_ctx_t ctx = {0};
    hu_channel_loop_state_t state = {0};
    hu_service_configure(&ctx, &state);
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_service_status());
    hu_service_stop();
    hu_service_configure(NULL, NULL);
}

void run_ported_modules_tests(void) {
    HU_TEST_SUITE("Ported Modules");
    HU_RUN_TEST(test_channel_catalog_all);
    HU_RUN_TEST(test_channel_catalog_find_by_key);
    HU_RUN_TEST(test_channel_catalog_parse_peer_kind);
    HU_RUN_TEST(test_config_mutator_path_requires_restart);
    HU_RUN_TEST(test_config_mutator_get_path_denied);
    HU_RUN_TEST(test_config_mutator_mutate_denied_path);
    HU_RUN_TEST(test_config_mutator_mutate_unset);
    HU_RUN_TEST(test_doctor_parse_df);
    HU_RUN_TEST(test_doctor_truncate_null_alloc);
    HU_RUN_TEST(test_doctor_truncate_null_string);
    HU_RUN_TEST(test_doctor_truncate_zero_len);
    HU_RUN_TEST(test_doctor_truncate_normal_truncation);
    HU_RUN_TEST(test_doctor_truncate_shorter_than_max);
    HU_RUN_TEST(test_doctor_truncate_null_out);
    HU_RUN_TEST(test_doctor_check_config_null_alloc);
    HU_RUN_TEST(test_doctor_check_config_null_cfg);
    HU_RUN_TEST(test_doctor_check_config_null_out);
    HU_RUN_TEST(test_doctor_check_config_valid_with_defaults);
    HU_RUN_TEST(test_agent_commands_parse);
    HU_RUN_TEST(test_agent_commands_bare_reset_prompt);
    HU_RUN_TEST(test_rate_tracker);
    HU_RUN_TEST(test_sandbox_create_noop);
    HU_RUN_TEST(test_capabilities_manifest);
    HU_RUN_TEST(test_config_mutator_mutate);
    HU_RUN_TEST(test_update_check_mock);
    HU_RUN_TEST(test_update_apply_mock);
    HU_RUN_TEST(test_service_start_stop);
    HU_RUN_TEST(test_service_configure_null);
    HU_RUN_TEST(test_service_double_start);
    HU_RUN_TEST(test_service_configure_with_ctx);
}
