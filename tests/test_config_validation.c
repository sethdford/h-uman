/* Config validation tests — unknown keys, type checks, value validation. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_config_validate_strict_valid_passes(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json =
        "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4o\",\"max_tokens\":2048,"
        "\"gateway\":{\"port\":3000}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, false);
    HU_ASSERT_EQ(verr, HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_validate_strict_unknown_key_warning(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    /* Parse JSON with unknown key — validation runs inside parse, should not fail */
    const char *json =
        "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4\",\"gateway\":{\"port\":3000},"
        "\"typo_key\":123}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_validate_strict_wrong_type_returns_error_in_strict(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    /* Build minimal JSON with wrong type for default_provider (number instead of string) */
    hu_json_value_t *root = hu_json_object_new(&cfg.allocator);
    HU_ASSERT_NOT_NULL(root);
    hu_json_object_set(&cfg.allocator, root, "default_provider",
                       hu_json_number_new(&cfg.allocator, 123));
    hu_json_object_set(&cfg.allocator, root, "default_model",
                       hu_json_string_new(&cfg.allocator, "gpt-4", 5));
    /* Set cfg defaults so value validation passes */
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    hu_error_t verr = hu_config_validate_strict(&cfg, root, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
    hu_json_free(&cfg.allocator, root);
    hu_arena_destroy(arena);
}

static void test_config_validate_strict_invalid_url_https_required(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    /* Simulate http:// URL in memory config */
    char url[] = "http://example.com/api";
    cfg.memory.api_base_url = url;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_extreme_numeric_warning(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    cfg.max_tokens = 2000000; /* over 1000000 */
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_path_traversal_rejected(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    char path[] = "/home/../etc/passwd";
    cfg.workspace_dir = path;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_non_strict_warnings_only(void) {
    /* With strict=false, invalid URL should warn but not fail */
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    char url[] = "http://insecure.example.com";
    cfg.memory.api_base_url = url;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, false);
    HU_ASSERT_EQ(verr, HU_OK);
}

static void test_config_validate_strict_unknown_provider_fails_in_strict(void) {
    /* Parse then validate - unknown provider with strict=true must fail */
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"unknown_fake_provider\",\"default_model\":\"x\","
                       "\"gateway\":{\"port\":3000}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
    hu_arena_destroy(arena);
}

static void test_config_validate_strict_security_autonomy_level_over_max_fails(void) {
    /* security.autonomy_level must be 0–4 (JSON key autonomy_level under security). */
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    cfg.security.autonomy_level = 5;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_gateway_port_zero_fails(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 0;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_strict_empty_default_provider_fails(void) {
    char empty[] = "";
    hu_config_t cfg = {0};
    cfg.default_provider = empty;
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}

#if !HU_ENABLE_SQLITE
static void test_config_validate_strict_memory_sqlite_backend_without_sqlite_build_fails(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    char backend[] = "sqlite";
    cfg.memory.backend = backend;
    hu_error_t verr = hu_config_validate_strict(&cfg, NULL, true);
    HU_ASSERT_EQ(verr, HU_ERR_CONFIG_INVALID);
}
#endif

/* Regression tests for 2026-05-18: reaction_collection and personalization
 * were missing from hu_config_top_keys in src/config_validate.c. The parser
 * in src/config_parse.c handles both (parse_reaction_collection at L1302),
 * but check_unknown_top_keys logged "unknown key: 'reaction_collection'
 * (ignored)" on every daemon startup AND, in strict mode, would have failed
 * validation outright. These two tests pin the whitelist additions so a
 * future contributor can't drop either key without a failing test.
 * See: .claude/rules/silent-config-gated-subsystems.md +
 *      docs/plans/2026-05-18-phase5-eval-honest-status.md */
static void test_config_validate_strict_reaction_collection_is_known_top_key(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    /* gateway.port is required by the strict base-check (see end of
     * hu_config_validate_strict in src/config_validate.c). Without it,
     * strict mode would fail with HU_ERR_CONFIG_INVALID regardless of
     * the whitelist state and this test would NOT actually exercise the
     * whitelist behaviour we want to pin. */
    const char *json = "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4o\","
                       "\"gateway\":{\"port\":3000},"
                       "\"reaction_collection\":{\"enabled\":true,\"poll_interval_seconds\":30,"
                       "\"channels\":[\"imessage\"]}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_value_t *root = NULL;
    err = hu_json_parse(&cfg.allocator, json, strlen(json), &root);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(root);
    /* strict=true MUST accept reaction_collection. If this assertion fires
     * with HU_ERR_*, someone has removed reaction_collection from
     * hu_config_top_keys in src/config_validate.c — restore it and re-read
     * the 2026-05-18 audit. */
    hu_error_t verr = hu_config_validate_strict(&cfg, root, true);
    HU_ASSERT_EQ(verr, HU_OK);
    /* And the parsed config should have the block populated */
    HU_ASSERT_EQ((int)cfg.reaction_collection.enabled, (int)true);
    hu_arena_destroy(arena);
}

static void test_config_validate_strict_personalization_is_known_top_key(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"openai\",\"default_model\":\"gpt-4o\","
                       "\"gateway\":{\"port\":3000},"
                       "\"personalization\":{\"enabled\":true}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_value_t *root = NULL;
    err = hu_json_parse(&cfg.allocator, json, strlen(json), &root);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(root);
    /* strict=true MUST accept personalization. Same rationale as the
     * reaction_collection sibling test. */
    hu_error_t verr = hu_config_validate_strict(&cfg, root, true);
    HU_ASSERT_EQ(verr, HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_validate_strict_unknown_key_fails_with_root(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"openai\",\"bogus_unknown_key\":true}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_value_t *root = NULL;
    err = hu_json_parse(&cfg.allocator, json, strlen(json), &root);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(root);
    hu_error_t verr = hu_config_validate_strict(&cfg, root, true);
    HU_ASSERT_NEQ(verr, HU_OK);
    hu_arena_destroy(arena);
}

void run_config_validation_tests(void) {
    HU_TEST_SUITE("Config validation");
    HU_RUN_TEST(test_config_validate_strict_valid_passes);
    HU_RUN_TEST(test_config_validate_strict_unknown_key_warning);
    HU_RUN_TEST(test_config_validate_strict_wrong_type_returns_error_in_strict);
    HU_RUN_TEST(test_config_validate_strict_invalid_url_https_required);
    HU_RUN_TEST(test_config_validate_strict_extreme_numeric_warning);
    HU_RUN_TEST(test_config_validate_strict_path_traversal_rejected);
    HU_RUN_TEST(test_config_validate_strict_non_strict_warnings_only);
    HU_RUN_TEST(test_config_validate_strict_unknown_provider_fails_in_strict);
    HU_RUN_TEST(test_config_validate_strict_security_autonomy_level_over_max_fails);
    HU_RUN_TEST(test_config_validate_strict_gateway_port_zero_fails);
    HU_RUN_TEST(test_config_validate_strict_empty_default_provider_fails);
#if !HU_ENABLE_SQLITE
    HU_RUN_TEST(test_config_validate_strict_memory_sqlite_backend_without_sqlite_build_fails);
#endif
    HU_RUN_TEST(test_config_validate_strict_unknown_key_fails_with_root);
    HU_RUN_TEST(test_config_validate_strict_reaction_collection_is_known_top_key);
    HU_RUN_TEST(test_config_validate_strict_personalization_is_known_top_key);
}
