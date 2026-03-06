/* Config validation tests — unknown keys, type checks, value validation. */
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_config_validate_strict_valid_passes(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json =
        "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4o\",\"max_tokens\":2048,"
        "\"gateway\":{\"port\":3000}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    sc_error_t verr = sc_config_validate_strict(&cfg, NULL, false);
    SC_ASSERT_EQ(verr, SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_validate_strict_unknown_key_warning(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    /* Parse JSON with unknown key — validation runs inside parse, should not fail */
    const char *json =
        "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4\",\"gateway\":{\"port\":3000},"
        "\"typo_key\":123}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_validate_strict_wrong_type_returns_error_in_strict(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    /* Build minimal JSON with wrong type for default_provider (number instead of string) */
    sc_json_value_t *root = sc_json_object_new(&cfg.allocator);
    SC_ASSERT_NOT_NULL(root);
    sc_json_object_set(&cfg.allocator, root, "default_provider",
                       sc_json_number_new(&cfg.allocator, 123));
    sc_json_object_set(&cfg.allocator, root, "default_model",
                       sc_json_string_new(&cfg.allocator, "gpt-4", 5));
    /* Set cfg defaults so value validation passes */
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    sc_error_t verr = sc_config_validate_strict(&cfg, root, true);
    SC_ASSERT_EQ(verr, SC_ERR_CONFIG_INVALID);
    sc_json_free(&cfg.allocator, root);
    sc_arena_destroy(arena);
}

static void test_config_validate_strict_invalid_url_https_required(void) {
    sc_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    /* Simulate http:// URL in memory config */
    char url[] = "http://example.com/api";
    cfg.memory.api_base_url = url;
    sc_error_t verr = sc_config_validate_strict(&cfg, NULL, true);
    SC_ASSERT_EQ(verr, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_extreme_numeric_warning(void) {
    sc_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    cfg.max_tokens = 2000000; /* over 1000000 */
    sc_error_t verr = sc_config_validate_strict(&cfg, NULL, true);
    SC_ASSERT_EQ(verr, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_path_traversal_rejected(void) {
    sc_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    char path[] = "/home/../etc/passwd";
    cfg.workspace_dir = path;
    sc_error_t verr = sc_config_validate_strict(&cfg, NULL, true);
    SC_ASSERT_EQ(verr, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_non_strict_warnings_only(void) {
    /* With strict=false, invalid URL should warn but not fail */
    sc_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    char url[] = "http://insecure.example.com";
    cfg.memory.api_base_url = url;
    sc_error_t verr = sc_config_validate_strict(&cfg, NULL, false);
    SC_ASSERT_EQ(verr, SC_OK);
}

static void test_config_validate_strict_unknown_provider_fails_in_strict(void) {
    /* Parse then validate - unknown provider with strict=true must fail */
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"unknown_fake_provider\",\"default_model\":\"x\","
                       "\"gateway\":{\"port\":3000}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    sc_error_t verr = sc_config_validate_strict(&cfg, NULL, true);
    SC_ASSERT_EQ(verr, SC_ERR_CONFIG_INVALID);
    sc_arena_destroy(arena);
}

static void test_config_validate_strict_unknown_key_fails_with_root(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"openai\",\"bogus_unknown_key\":true}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    sc_json_value_t *root = NULL;
    err = sc_json_parse(&cfg.allocator, json, strlen(json), &root);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(root);
    sc_error_t verr = sc_config_validate_strict(&cfg, root, true);
    SC_ASSERT_NEQ(verr, SC_OK);
    sc_arena_destroy(arena);
}

void run_config_validation_tests(void) {
    SC_TEST_SUITE("Config validation");
    SC_RUN_TEST(test_config_validate_strict_valid_passes);
    SC_RUN_TEST(test_config_validate_strict_unknown_key_warning);
    SC_RUN_TEST(test_config_validate_strict_wrong_type_returns_error_in_strict);
    SC_RUN_TEST(test_config_validate_strict_invalid_url_https_required);
    SC_RUN_TEST(test_config_validate_strict_extreme_numeric_warning);
    SC_RUN_TEST(test_config_validate_strict_path_traversal_rejected);
    SC_RUN_TEST(test_config_validate_strict_non_strict_warnings_only);
    SC_RUN_TEST(test_config_validate_strict_unknown_provider_fails_in_strict);
    SC_RUN_TEST(test_config_validate_strict_unknown_key_fails_with_root);
}
