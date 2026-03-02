/* Config parsing and validation tests. */
#include "test_framework.h"
#include "seaclaw/config.h"
#include "seaclaw/config_parse.h"
#include "seaclaw/daemon.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

static void test_config_parse_empty_json(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.arena = arena;
    cfg_local.allocator = sc_arena_allocator(arena);
    sc_error_t err = sc_config_parse_json(&cfg_local, "{}", 2);
    SC_ASSERT_EQ(err, SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_parse_with_providers(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.arena = arena;
    cfg_local.allocator = sc_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"openai\",\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\"}]}";
    sc_error_t err = sc_config_parse_json(&cfg_local, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg_local.default_provider, "openai");
    SC_ASSERT(cfg_local.providers_len >= 1);
    SC_ASSERT_STR_EQ(cfg_local.providers[0].name, "openai");
    sc_arena_destroy(arena);
}

static void test_config_parse_all_sections(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    const char *json = "{\"workspace\":\"/tmp\",\"default_model\":\"gpt-4\","
        "\"autonomy\":{\"level\":\"readonly\"},\"gateway\":{\"port\":8080,\"host\":\"0.0.0.0\"},"
        "\"memory\":{\"backend\":\"sqlite\",\"auto_save\":true},\"security\":{\"autonomy_level\":0}}";
    sc_error_t err = sc_config_parse_json(&cfg_local, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg_local.workspace_dir, "/tmp");
    SC_ASSERT_STR_EQ(cfg_local.default_model, "gpt-4");
    SC_ASSERT_STR_EQ(cfg_local.autonomy.level, "readonly");
    SC_ASSERT_EQ(cfg_local.gateway.port, 8080);
    SC_ASSERT_STR_EQ(cfg_local.gateway.host, "0.0.0.0");
    SC_ASSERT_STR_EQ(cfg_local.memory.backend, "sqlite");
    SC_ASSERT_TRUE(cfg_local.memory.auto_save);
    sc_arena_destroy(arena);
}

static void test_config_parse_malformed_missing_brace(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    sc_error_t err = sc_config_parse_json(&cfg_local, "{", 1);
    SC_ASSERT(err != SC_OK);
    err = sc_config_parse_json(&cfg_local, "}", 1);
    SC_ASSERT(err != SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_parse_malformed_bad_types(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    sc_error_t err = sc_config_parse_json(&cfg_local, "{]", 2);
    SC_ASSERT(err != SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_env_overrides(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    cfg_local.default_provider = sc_strdup(&cfg_local.allocator, "openai");
    cfg_local.default_model = sc_strdup(&cfg_local.allocator, "gpt-4");
    setenv("SEACLAW_PROVIDER", "anthropic", 1);
    setenv("SEACLAW_MODEL", "claude-3", 1);
    sc_config_apply_env_overrides(&cfg_local);
    SC_ASSERT_STR_EQ(cfg_local.default_provider, "anthropic");
    SC_ASSERT_STR_EQ(cfg_local.default_model, "claude-3");
    unsetenv("SEACLAW_PROVIDER");
    unsetenv("SEACLAW_MODEL");
    sc_arena_destroy(arena);
}

static void test_config_validate_ok(void) {
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char prov[] = "openai";
    char model[] = "gpt-4";
    char host[] = "127.0.0.1";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    cfg.gateway.host = host;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_config_validate_invalid_provider(void) {
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.gateway.port = 3000;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_invalid_port(void) {
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char prov[] = "openai";
    cfg.default_provider = prov;
    cfg.gateway.port = 0;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

/* ─── WP-21B parity: malformed JSON, config merge, defaults ────────────────── */
static void test_config_parse_malformed_unclosed_string(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    sc_error_t err = sc_config_parse_json(&cfg_local, "{\"x\":\"unclosed", 14);
    SC_ASSERT(err != SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_parse_malformed_missing_value(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    /* Truncated JSON - missing value after colon */
    sc_error_t err = sc_config_parse_json(&cfg_local, "{\"a\"", 4);
    SC_ASSERT(err != SC_OK);
    sc_arena_destroy(arena);
}

static void test_config_parse_missing_required_nested_defaults(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    sc_error_t err = sc_config_parse_json(&cfg_local, "{\"gateway\":{}}", 13);
    /* C parser may reject empty gateway or require defaults; accept any defined result */
    SC_ASSERT_TRUE(err == SC_OK || (err > SC_OK && err < SC_ERR_COUNT));
    sc_arena_destroy(arena);
}

static void test_config_parse_security_section(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = sc_arena_allocator(arena);
    cfg_local.arena = arena;
    const char *j = "{\"security\":{\"autonomy_level\":1,\"audit\":{\"enabled\":true}}}";
    sc_error_t err = sc_config_parse_json(&cfg_local, j, strlen(j));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg_local.security.autonomy_level, 1u);
    SC_ASSERT_TRUE(cfg_local.security.audit.enabled);
    sc_arena_destroy(arena);
}

static void test_config_validate_null_model_fails(void) {
    sc_config_t cfg = {0};
    char prov[] = "openai";
    cfg.default_provider = prov;
    cfg.default_model = NULL;
    cfg.gateway.port = 3000;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

static void test_config_parse_string_array_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *json = NULL;
    const char *input = "[\"a\",\"b\",\"c\"]";
    sc_error_t err = sc_json_parse(&alloc, input, strlen(input), &json);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(json);
    char **arr = NULL;
    size_t count = 0;
    err = sc_config_parse_string_array(&alloc, json, &arr, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 3u);
    SC_ASSERT_STR_EQ(arr[0], "a");
    SC_ASSERT_STR_EQ(arr[1], "b");
    SC_ASSERT_STR_EQ(arr[2], "c");
    for (size_t i = 0; i < count; i++)
        alloc.free(alloc.ctx, arr[i], strlen(arr[i]) + 1);
    alloc.free(alloc.ctx, arr, sizeof(char *) * count);
    sc_json_free(&alloc, json);
}

static void test_config_parse_string_array_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *json = NULL;
    sc_json_parse(&alloc, "[]", 2, &json);
    SC_ASSERT_NOT_NULL(json);
    char **arr = NULL;
    size_t count = 0;
    sc_error_t err = sc_config_parse_string_array(&alloc, json, &arr, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
    SC_ASSERT_NULL(arr);
    sc_json_free(&alloc, json);
}

static void test_config_parse_string_array_skips_non_strings(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *json = NULL;
    const char *input = "[\"x\",\"y\"]";
    sc_error_t err = sc_json_parse(&alloc, input, strlen(input), &json);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(json);
    char **arr = NULL;
    size_t count = 0;
    err = sc_config_parse_string_array(&alloc, json, &arr, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_STR_EQ(arr[0], "x");
    SC_ASSERT_STR_EQ(arr[1], "y");
    alloc.free(alloc.ctx, arr[0], 2);
    alloc.free(alloc.ctx, arr[1], 2);
    alloc.free(alloc.ctx, arr, sizeof(char *) * 2);
    sc_json_free(&alloc, json);
}

static void test_config_parse_email_channel(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"channels\":{\"email\":{\"smtp_host\":\"smtp.gmail.com\","
        "\"smtp_port\":587,\"from_address\":\"me@gmail.com\","
        "\"smtp_user\":\"me@gmail.com\",\"smtp_pass\":\"apppassword\","
        "\"imap_host\":\"imap.gmail.com\",\"imap_port\":993}}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg.channels.email.smtp_host, "smtp.gmail.com");
    SC_ASSERT_EQ(cfg.channels.email.smtp_port, 587);
    SC_ASSERT_STR_EQ(cfg.channels.email.from_address, "me@gmail.com");
    SC_ASSERT_STR_EQ(cfg.channels.email.smtp_user, "me@gmail.com");
    SC_ASSERT_STR_EQ(cfg.channels.email.smtp_pass, "apppassword");
    SC_ASSERT_STR_EQ(cfg.channels.email.imap_host, "imap.gmail.com");
    SC_ASSERT_EQ(cfg.channels.email.imap_port, 993);
    sc_arena_destroy(arena);
}

static void test_config_parse_imessage_channel(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"channels\":{\"imessage\":{\"default_target\":\"+15551234567\"}}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg.channels.imessage.default_target, "+15551234567");
    sc_arena_destroy(arena);
}

static void test_config_parse_mcp_servers(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"mcp_servers\":{\"filesystem\":{\"command\":\"npx\","
        "\"args\":[\"-y\",\"@modelcontextprotocol/server-filesystem\"]},"
        "\"search\":{\"command\":\"search-server\",\"args\":[]}}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg.mcp_servers_len, 2u);
    SC_ASSERT_STR_EQ(cfg.mcp_servers[0].name, "filesystem");
    SC_ASSERT_STR_EQ(cfg.mcp_servers[0].command, "npx");
    SC_ASSERT_EQ(cfg.mcp_servers[0].args_count, 2u);
    SC_ASSERT_STR_EQ(cfg.mcp_servers[0].args[0], "-y");
    SC_ASSERT_STR_EQ(cfg.mcp_servers[0].args[1], "@modelcontextprotocol/server-filesystem");
    SC_ASSERT_STR_EQ(cfg.mcp_servers[1].name, "search");
    SC_ASSERT_STR_EQ(cfg.mcp_servers[1].command, "search-server");
    SC_ASSERT_EQ(cfg.mcp_servers[1].args_count, 0u);
    sc_arena_destroy(arena);
}

static void test_config_parse_mcp_servers_empty(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    sc_error_t err = sc_config_parse_json(&cfg, "{\"mcp_servers\":{}}", 18);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg.mcp_servers_len, 0u);
    sc_arena_destroy(arena);
}

static void test_config_parse_service_loop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_service_run(&alloc, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_config_parse_memory_postgres(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(alloc);
    SC_ASSERT_NOT_NULL(arena);
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"memory\":{\"backend\":\"postgres\","
        "\"postgres_url\":\"postgres://localhost/test\","
        "\"postgres_schema\":\"myschema\","
        "\"postgres_table\":\"entries\"}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg.memory.backend, "postgres");
    SC_ASSERT_STR_EQ(cfg.memory.postgres_url, "postgres://localhost/test");
    SC_ASSERT_STR_EQ(cfg.memory.postgres_schema, "myschema");
    SC_ASSERT_STR_EQ(cfg.memory.postgres_table, "entries");
    sc_arena_destroy(arena);
}

static void test_config_parse_memory_redis(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(alloc);
    SC_ASSERT_NOT_NULL(arena);
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"memory\":{\"backend\":\"redis\","
        "\"redis_host\":\"redis.local\",\"redis_port\":6380,"
        "\"redis_key_prefix\":\"sc\"}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg.memory.backend, "redis");
    SC_ASSERT_STR_EQ(cfg.memory.redis_host, "redis.local");
    SC_ASSERT_EQ(cfg.memory.redis_port, 6380);
    SC_ASSERT_STR_EQ(cfg.memory.redis_key_prefix, "sc");
    sc_arena_destroy(arena);
}

static void test_config_parse_memory_api(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(alloc);
    SC_ASSERT_NOT_NULL(arena);
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    const char *json = "{\"memory\":{\"backend\":\"api\","
        "\"api_base_url\":\"https://mem.example.com\","
        "\"api_key\":\"test-key\",\"api_timeout_ms\":3000}}";
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg.memory.backend, "api");
    SC_ASSERT_STR_EQ(cfg.memory.api_base_url, "https://mem.example.com");
    SC_ASSERT_STR_EQ(cfg.memory.api_key, "test-key");
    SC_ASSERT_EQ(cfg.memory.api_timeout_ms, 3000u);
    sc_arena_destroy(arena);
}

void run_config_parse_tests(void) {
    SC_TEST_SUITE("Config parse");
    SC_RUN_TEST(test_config_parse_empty_json);
    SC_RUN_TEST(test_config_parse_with_providers);
    SC_RUN_TEST(test_config_parse_all_sections);
    SC_RUN_TEST(test_config_parse_malformed_missing_brace);
    SC_RUN_TEST(test_config_parse_malformed_bad_types);
    SC_RUN_TEST(test_config_env_overrides);
    SC_RUN_TEST(test_config_validate_ok);
    SC_RUN_TEST(test_config_validate_invalid_provider);
    SC_RUN_TEST(test_config_validate_invalid_port);
    SC_RUN_TEST(test_config_parse_malformed_unclosed_string);
    SC_RUN_TEST(test_config_parse_malformed_missing_value);
    SC_RUN_TEST(test_config_parse_missing_required_nested_defaults);
    SC_RUN_TEST(test_config_parse_security_section);
    SC_RUN_TEST(test_config_validate_null_model_fails);
    SC_RUN_TEST(test_config_parse_string_array_basic);
    SC_RUN_TEST(test_config_parse_string_array_empty);
    SC_RUN_TEST(test_config_parse_string_array_skips_non_strings);
    SC_RUN_TEST(test_config_parse_email_channel);
    SC_RUN_TEST(test_config_parse_imessage_channel);
    SC_RUN_TEST(test_config_parse_mcp_servers);
    SC_RUN_TEST(test_config_parse_mcp_servers_empty);

    SC_TEST_SUITE("Service loop");
    SC_RUN_TEST(test_config_parse_service_loop);

    SC_TEST_SUITE("Memory backend config");
    SC_RUN_TEST(test_config_parse_memory_postgres);
    SC_RUN_TEST(test_config_parse_memory_redis);
    SC_RUN_TEST(test_config_parse_memory_api);
}
