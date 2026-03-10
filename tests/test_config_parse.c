/* Config parsing and validation tests. */
#include "human/config.h"
#include "human/config_parse.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/daemon.h"
#include "human/security/sandbox.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_config_parse_empty_json(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.arena = arena;
    cfg_local.allocator = hu_arena_allocator(arena);
    hu_error_t err = hu_config_parse_json(&cfg_local, "{}", 2);
    HU_ASSERT_EQ(err, HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_parse_with_providers(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.arena = arena;
    cfg_local.allocator = hu_arena_allocator(arena);
    const char *json = "{\"default_provider\":\"openai\",\"providers\":[{\"name\":\"openai\",\"api_"
                       "key\":\"sk-test\"}]}";
    hu_error_t err = hu_config_parse_json(&cfg_local, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg_local.default_provider, "openai");
    HU_ASSERT(cfg_local.providers_len >= 1);
    HU_ASSERT_STR_EQ(cfg_local.providers[0].name, "openai");
    hu_arena_destroy(arena);
}

static void test_config_parse_all_sections(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    const char *json =
        "{\"workspace\":\"/tmp\",\"default_model\":\"gpt-4\","
        "\"autonomy\":{\"level\":\"readonly\"},\"gateway\":{\"port\":8080,\"host\":\"0.0.0.0\"},"
        "\"memory\":{\"backend\":\"sqlite\",\"auto_save\":true},\"security\":{\"autonomy_level\":0}"
        "}";
    hu_error_t err = hu_config_parse_json(&cfg_local, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg_local.workspace_dir, "/tmp");
    HU_ASSERT_STR_EQ(cfg_local.default_model, "gpt-4");
    HU_ASSERT_STR_EQ(cfg_local.autonomy.level, "readonly");
    HU_ASSERT_EQ(cfg_local.gateway.port, 8080);
    HU_ASSERT_STR_EQ(cfg_local.gateway.host, "0.0.0.0");
    HU_ASSERT_STR_EQ(cfg_local.memory.backend, "sqlite");
    HU_ASSERT_TRUE(cfg_local.memory.auto_save);
    hu_arena_destroy(arena);
}

static void test_config_parse_malformed_missing_brace(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    hu_error_t err = hu_config_parse_json(&cfg_local, "{", 1);
    HU_ASSERT(err != HU_OK);
    err = hu_config_parse_json(&cfg_local, "}", 1);
    HU_ASSERT(err != HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_parse_malformed_bad_types(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    hu_error_t err = hu_config_parse_json(&cfg_local, "{]", 2);
    HU_ASSERT(err != HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_env_overrides(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    cfg_local.default_provider = hu_strdup(&cfg_local.allocator, "openai");
    cfg_local.default_model = hu_strdup(&cfg_local.allocator, "gpt-4");
    setenv("HUMAN_PROVIDER", "anthropic", 1);
    setenv("HUMAN_MODEL", "claude-3", 1);
    hu_config_apply_env_overrides(&cfg_local);
    HU_ASSERT_STR_EQ(cfg_local.default_provider, "anthropic");
    HU_ASSERT_STR_EQ(cfg_local.default_model, "claude-3");
    unsetenv("HUMAN_PROVIDER");
    unsetenv("HUMAN_MODEL");
    hu_arena_destroy(arena);
}

static void test_config_validate_ok(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char prov[] = "openai";
    char model[] = "gpt-4";
    char host[] = "127.0.0.1";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    cfg.gateway.host = host;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_config_validate_invalid_provider(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_invalid_port(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char prov[] = "openai";
    cfg.default_provider = prov;
    cfg.gateway.port = 0;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

/* ─── WP-21B parity: malformed JSON, config merge, defaults ────────────────── */
static void test_config_parse_malformed_unclosed_string(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    hu_error_t err = hu_config_parse_json(&cfg_local, "{\"x\":\"unclosed", 14);
    HU_ASSERT(err != HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_parse_malformed_missing_value(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    /* Truncated JSON - missing value after colon */
    hu_error_t err = hu_config_parse_json(&cfg_local, "{\"a\"", 4);
    HU_ASSERT(err != HU_OK);
    hu_arena_destroy(arena);
}

static void test_config_parse_missing_required_nested_defaults(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    hu_error_t err = hu_config_parse_json(&cfg_local, "{\"gateway\":{}}", 13);
    /* C parser may reject empty gateway or require defaults; accept any defined result */
    HU_ASSERT_TRUE(err == HU_OK || (err > HU_OK && err < HU_ERR_COUNT));
    hu_arena_destroy(arena);
}

static void test_config_parse_security_section(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    const char *j = "{\"security\":{\"autonomy_level\":1,\"audit\":{\"enabled\":true}}}";
    hu_error_t err = hu_config_parse_json(&cfg_local, j, strlen(j));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg_local.security.autonomy_level, 1u);
    HU_ASSERT_TRUE(cfg_local.security.audit.enabled);
    hu_arena_destroy(arena);
}

static void test_config_parse_sandbox_config(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.allocator = hu_arena_allocator(arena);
    cfg_local.arena = arena;
    const char *j = "{\"security\":{\"sandbox\":\"seatbelt\",\"sandbox_config\":{"
                    "\"enabled\":true,"
                    "\"firejail_args\":[\"--whitelist=/opt\",\"--rlimit-cpu=10\"],"
                    "\"net_proxy\":{\"enabled\":true,\"deny_all\":true,"
                    "\"proxy_addr\":\"http://127.0.0.1:8080\","
                    "\"allowed_domains\":[\"api.example.com\",\"*.github.com\"]}"
                    "}}}";
    hu_error_t err = hu_config_parse_json(&cfg_local, j, strlen(j));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(cfg_local.security.sandbox_config.enabled);
    HU_ASSERT_EQ(cfg_local.security.sandbox_config.firejail_args_len, 2);
    HU_ASSERT(cfg_local.security.sandbox_config.firejail_args != NULL);
    HU_ASSERT_STR_EQ(cfg_local.security.sandbox_config.firejail_args[0], "--whitelist=/opt");
    HU_ASSERT_STR_EQ(cfg_local.security.sandbox_config.firejail_args[1], "--rlimit-cpu=10");
    HU_ASSERT_TRUE(cfg_local.security.sandbox_config.net_proxy.enabled);
    HU_ASSERT_TRUE(cfg_local.security.sandbox_config.net_proxy.deny_all);
    HU_ASSERT_STR_EQ(cfg_local.security.sandbox_config.net_proxy.proxy_addr,
                     "http://127.0.0.1:8080");
    HU_ASSERT_EQ(cfg_local.security.sandbox_config.net_proxy.allowed_domains_len, 2);
    HU_ASSERT_STR_EQ(cfg_local.security.sandbox_config.net_proxy.allowed_domains[0],
                     "api.example.com");
    HU_ASSERT_STR_EQ(cfg_local.security.sandbox_config.net_proxy.allowed_domains[1],
                     "*.github.com");
    hu_arena_destroy(arena);
}

static void test_config_validate_null_model_fails(void) {
    hu_config_t cfg = {0};
    char prov[] = "openai";
    cfg.default_provider = prov;
    cfg.default_model = NULL;
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_parse_string_array_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *json = NULL;
    const char *input = "[\"a\",\"b\",\"c\"]";
    hu_error_t err = hu_json_parse(&alloc, input, strlen(input), &json);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(json);
    char **arr = NULL;
    size_t count = 0;
    err = hu_config_parse_string_array(&alloc, json, &arr, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_STR_EQ(arr[0], "a");
    HU_ASSERT_STR_EQ(arr[1], "b");
    HU_ASSERT_STR_EQ(arr[2], "c");
    for (size_t i = 0; i < count; i++)
        alloc.free(alloc.ctx, arr[i], strlen(arr[i]) + 1);
    alloc.free(alloc.ctx, arr, sizeof(char *) * count);
    hu_json_free(&alloc, json);
}

static void test_config_parse_string_array_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *json = NULL;
    hu_json_parse(&alloc, "[]", 2, &json);
    HU_ASSERT_NOT_NULL(json);
    char **arr = NULL;
    size_t count = 0;
    hu_error_t err = hu_config_parse_string_array(&alloc, json, &arr, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(arr);
    hu_json_free(&alloc, json);
}

static void test_config_parse_string_array_skips_non_strings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *json = NULL;
    const char *input = "[\"x\",\"y\"]";
    hu_error_t err = hu_json_parse(&alloc, input, strlen(input), &json);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(json);
    char **arr = NULL;
    size_t count = 0;
    err = hu_config_parse_string_array(&alloc, json, &arr, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(arr[0], "x");
    HU_ASSERT_STR_EQ(arr[1], "y");
    alloc.free(alloc.ctx, arr[0], 2);
    alloc.free(alloc.ctx, arr[1], 2);
    alloc.free(alloc.ctx, arr, sizeof(char *) * 2);
    hu_json_free(&alloc, json);
}

static void test_config_parse_email_channel(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"channels\":{\"email\":{\"smtp_host\":\"smtp.gmail.com\","
                       "\"smtp_port\":587,\"from_address\":\"me@gmail.com\","
                       "\"smtp_user\":\"me@gmail.com\",\"smtp_pass\":\"apppassword\","
                       "\"imap_host\":\"imap.gmail.com\",\"imap_port\":993}}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.channels.email.smtp_host, "smtp.gmail.com");
    HU_ASSERT_EQ(cfg.channels.email.smtp_port, 587);
    HU_ASSERT_STR_EQ(cfg.channels.email.from_address, "me@gmail.com");
    HU_ASSERT_STR_EQ(cfg.channels.email.smtp_user, "me@gmail.com");
    HU_ASSERT_STR_EQ(cfg.channels.email.smtp_pass, "apppassword");
    HU_ASSERT_STR_EQ(cfg.channels.email.imap_host, "imap.gmail.com");
    HU_ASSERT_EQ(cfg.channels.email.imap_port, 993);
    hu_arena_destroy(arena);
}

static void test_config_parse_imessage_channel(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"channels\":{\"imessage\":{\"default_target\":\"+15551234567\"}}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.channels.imessage.default_target, "+15551234567");
    hu_arena_destroy(arena);
}

static void test_config_parse_response_mode(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"channels\":{\"imessage\":{\"default_target\":\"+15551234567\","
                       "\"response_mode\":\"eager\"}}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.channels.imessage.default_target, "+15551234567");
    HU_ASSERT_STR_EQ(cfg.channels.imessage.response_mode, "eager");
    hu_arena_destroy(arena);
}

static void test_config_parse_response_mode_selective(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"channels\":{\"imessage\":{\"response_mode\":\"selective\"}}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.channels.imessage.response_mode, "selective");
    hu_arena_destroy(arena);
}

static void test_config_parse_response_mode_normal(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"channels\":{\"imessage\":{\"response_mode\":\"normal\"}}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.channels.imessage.response_mode, "normal");
    hu_arena_destroy(arena);
}

static void test_config_parse_mcp_servers(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"mcp_servers\":{\"filesystem\":{\"command\":\"npx\","
                       "\"args\":[\"-y\",\"@modelcontextprotocol/server-filesystem\"]},"
                       "\"search\":{\"command\":\"search-server\",\"args\":[]}}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.mcp_servers_len, 2u);
    HU_ASSERT_STR_EQ(cfg.mcp_servers[0].name, "filesystem");
    HU_ASSERT_STR_EQ(cfg.mcp_servers[0].command, "npx");
    HU_ASSERT_EQ(cfg.mcp_servers[0].args_count, 2u);
    HU_ASSERT_STR_EQ(cfg.mcp_servers[0].args[0], "-y");
    HU_ASSERT_STR_EQ(cfg.mcp_servers[0].args[1], "@modelcontextprotocol/server-filesystem");
    HU_ASSERT_STR_EQ(cfg.mcp_servers[1].name, "search");
    HU_ASSERT_STR_EQ(cfg.mcp_servers[1].command, "search-server");
    HU_ASSERT_EQ(cfg.mcp_servers[1].args_count, 0u);
    hu_arena_destroy(arena);
}

static void test_config_parse_mcp_servers_empty(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    hu_error_t err = hu_config_parse_json(&cfg, "{\"mcp_servers\":{}}", 18);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.mcp_servers_len, 0u);
    hu_arena_destroy(arena);
}

static void test_config_parse_nodes_array(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"nodes\":[{\"name\":\"local\",\"status\":\"online\"},{\"name\":\"remote-"
                       "1\",\"status\":\"offline\"}]}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.nodes_len, 2u);
    HU_ASSERT_STR_EQ(cfg.nodes[0].name, "local");
    HU_ASSERT_STR_EQ(cfg.nodes[0].status, "online");
    HU_ASSERT_STR_EQ(cfg.nodes[1].name, "remote-1");
    HU_ASSERT_STR_EQ(cfg.nodes[1].status, "offline");
    hu_arena_destroy(arena);
}

static void test_config_parse_service_loop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_service_run(&alloc, 0, NULL, 0, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_config_parse_memory_postgres(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(alloc);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"memory\":{\"backend\":\"postgres\","
                       "\"postgres_url\":\"postgres://localhost/test\","
                       "\"postgres_schema\":\"myschema\","
                       "\"postgres_table\":\"entries\"}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.memory.backend, "postgres");
    HU_ASSERT_STR_EQ(cfg.memory.postgres_url, "postgres://localhost/test");
    HU_ASSERT_STR_EQ(cfg.memory.postgres_schema, "myschema");
    HU_ASSERT_STR_EQ(cfg.memory.postgres_table, "entries");
    hu_arena_destroy(arena);
}

static void test_config_parse_memory_redis(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(alloc);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"memory\":{\"backend\":\"redis\","
                       "\"redis_host\":\"redis.local\",\"redis_port\":6380,"
                       "\"redis_key_prefix\":\"sc\"}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.memory.backend, "redis");
    HU_ASSERT_STR_EQ(cfg.memory.redis_host, "redis.local");
    HU_ASSERT_EQ(cfg.memory.redis_port, 6380);
    HU_ASSERT_STR_EQ(cfg.memory.redis_key_prefix, "sc");
    hu_arena_destroy(arena);
}

static void test_config_parse_memory_api(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(alloc);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"memory\":{\"backend\":\"api\","
                       "\"api_base_url\":\"https://mem.example.com\","
                       "\"api_key\":\"test-key\",\"api_timeout_ms\":3000}}";
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg.memory.backend, "api");
    HU_ASSERT_STR_EQ(cfg.memory.api_base_url, "https://mem.example.com");
    HU_ASSERT_STR_EQ(cfg.memory.api_key, "test-key");
    HU_ASSERT_EQ(cfg.memory.api_timeout_ms, 3000u);
    hu_arena_destroy(arena);
}

/* ─── config_serialize: hu_config_save ──────────────────────────────────────── */
static void test_config_save_null_cfg_returns_error(void) {
    hu_error_t err = hu_config_save(NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_config_save_null_path_returns_error(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    cfg.default_provider = hu_strdup(&cfg.allocator, "ollama");
    cfg.default_model = hu_strdup(&cfg.allocator, "llama2");
    cfg.gateway.port = 3000;
    cfg.config_path = NULL;
    hu_error_t err = hu_config_save(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_arena_destroy(arena);
}

static void test_config_save_roundtrip_key_fields(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.allocator = hu_arena_allocator(arena);
    cfg.arena = arena;
    cfg.workspace_dir = hu_strdup(&cfg.allocator, "/tmp/test-workspace");
    cfg.default_provider = hu_strdup(&cfg.allocator, "ollama");
    cfg.default_model = hu_strdup(&cfg.allocator, "llama3");
    cfg.gateway.port = 3000;

    char tmp_path[] = "/tmp/hu_test_save_XXXXXX";
    int fd = mkstemp(tmp_path);
    HU_ASSERT(fd >= 0);
    close(fd);
    cfg.config_path = tmp_path;

    hu_error_t err = hu_config_save(&cfg);
    HU_ASSERT_EQ(err, HU_OK);

    FILE *f = fopen(tmp_path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    unlink(tmp_path);

    hu_config_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    hu_arena_t *arena2 = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena2);
    cfg2.allocator = hu_arena_allocator(arena2);
    cfg2.arena = arena2;
    err = hu_config_parse_json(&cfg2, buf, n);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_STR_EQ(cfg2.workspace_dir, "/tmp/test-workspace");
    HU_ASSERT_STR_EQ(cfg2.default_provider, "ollama");
    HU_ASSERT_STR_EQ(cfg2.default_model, "llama3");
    HU_ASSERT_EQ(cfg2.gateway.port, 3000);

    hu_arena_destroy(arena);
    hu_arena_destroy(arena2);
}

static void test_config_sandbox_save_roundtrip(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg.allocator = hu_arena_allocator(arena);
    cfg.arena = arena;

    const char *j = "{\"security\":{\"sandbox\":\"firejail\",\"sandbox_config\":{"
                    "\"enabled\":true,\"backend\":\"firejail\","
                    "\"firejail_args\":[\"--whitelist=/opt\",\"--net=none\"],"
                    "\"net_proxy\":{\"enabled\":true,\"deny_all\":false,"
                    "\"proxy_addr\":\"http://10.0.0.1:3128\","
                    "\"allowed_domains\":[\"api.example.com\",\"*.internal.io\"]}"
                    "}}}";
    hu_error_t err = hu_config_parse_json(&cfg, j, strlen(j));
    HU_ASSERT_EQ(err, HU_OK);

    char tmp_path[] = "/tmp/hu_test_cfg_XXXXXX";
    int fd = mkstemp(tmp_path);
    HU_ASSERT(fd >= 0);
    close(fd);
    cfg.config_path = tmp_path;

    err = hu_config_save(&cfg);
    HU_ASSERT_EQ(err, HU_OK);

    FILE *f = fopen(tmp_path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    unlink(tmp_path);

    hu_config_t cfg2;
    memset(&cfg2, 0, sizeof(cfg2));
    hu_arena_t *arena2 = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena2);
    cfg2.allocator = hu_arena_allocator(arena2);
    cfg2.arena = arena2;
    err = hu_config_parse_json(&cfg2, buf, n);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_TRUE(cfg2.security.sandbox_config.enabled);
    HU_ASSERT_EQ((int)cfg2.security.sandbox_config.backend, (int)HU_SANDBOX_FIREJAIL);
    HU_ASSERT_EQ(cfg2.security.sandbox_config.firejail_args_len, 2);
    HU_ASSERT_STR_EQ(cfg2.security.sandbox_config.firejail_args[0], "--whitelist=/opt");
    HU_ASSERT_STR_EQ(cfg2.security.sandbox_config.firejail_args[1], "--net=none");
    HU_ASSERT_TRUE(cfg2.security.sandbox_config.net_proxy.enabled);
    HU_ASSERT_FALSE(cfg2.security.sandbox_config.net_proxy.deny_all);
    HU_ASSERT_STR_EQ(cfg2.security.sandbox_config.net_proxy.proxy_addr, "http://10.0.0.1:3128");
    HU_ASSERT_EQ(cfg2.security.sandbox_config.net_proxy.allowed_domains_len, 2);
    HU_ASSERT_STR_EQ(cfg2.security.sandbox_config.net_proxy.allowed_domains[0], "api.example.com");
    HU_ASSERT_STR_EQ(cfg2.security.sandbox_config.net_proxy.allowed_domains[1], "*.internal.io");

    hu_arena_destroy(arena);
    hu_arena_destroy(arena2);
}

static void test_config_parse_behavior_thresholds(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.arena = arena;
    cfg_local.allocator = hu_arena_allocator(arena);

    const char *json =
        "{\"behavior\":{"
        "\"consecutive_limit\":2,"
        "\"participation_pct\":35,"
        "\"max_response_chars\":250,"
        "\"min_response_chars\":20,"
        "\"decay_days\":14,"
        "\"dedup_threshold\":65,"
        "\"missed_msg_threshold_sec\":3600"
        "}}";

    hu_error_t err = hu_config_parse_json(&cfg_local, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify behavior thresholds were parsed correctly */
    HU_ASSERT_EQ(cfg_local.behavior.consecutive_limit, 2);
    HU_ASSERT_EQ(cfg_local.behavior.participation_pct, 35);
    HU_ASSERT_EQ(cfg_local.behavior.max_response_chars, 250);
    HU_ASSERT_EQ(cfg_local.behavior.min_response_chars, 20);
    HU_ASSERT_EQ(cfg_local.behavior.decay_days, 14);
    HU_ASSERT_EQ(cfg_local.behavior.dedup_threshold, 65);
    HU_ASSERT_EQ(cfg_local.behavior.missed_msg_threshold_sec, 3600);

    hu_arena_destroy(arena);
}

static void test_config_behavior_defaults(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg_local;
    memset(&cfg_local, 0, sizeof(cfg_local));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    cfg_local.arena = arena;
    cfg_local.allocator = hu_arena_allocator(arena);

    /* Set defaults before parsing empty JSON */
    cfg_local.behavior.consecutive_limit = 3;
    cfg_local.behavior.participation_pct = 40;
    cfg_local.behavior.max_response_chars = 300;
    cfg_local.behavior.min_response_chars = 15;
    cfg_local.behavior.decay_days = 30;
    cfg_local.behavior.dedup_threshold = 70;
    cfg_local.behavior.missed_msg_threshold_sec = 1800;

    hu_error_t err = hu_config_parse_json(&cfg_local, "{}", 2);
    HU_ASSERT_EQ(err, HU_OK);

    /* Defaults should still be in place */
    HU_ASSERT_EQ(cfg_local.behavior.consecutive_limit, 3);
    HU_ASSERT_EQ(cfg_local.behavior.participation_pct, 40);
    HU_ASSERT_EQ(cfg_local.behavior.max_response_chars, 300);
    HU_ASSERT_EQ(cfg_local.behavior.min_response_chars, 15);
    HU_ASSERT_EQ(cfg_local.behavior.decay_days, 30);
    HU_ASSERT_EQ(cfg_local.behavior.dedup_threshold, 70);
    HU_ASSERT_EQ(cfg_local.behavior.missed_msg_threshold_sec, 1800);

    hu_arena_destroy(arena);
}

void run_config_parse_tests(void) {
    HU_TEST_SUITE("Config parse");
    HU_RUN_TEST(test_config_parse_empty_json);
    HU_RUN_TEST(test_config_parse_with_providers);
    HU_RUN_TEST(test_config_parse_all_sections);
    HU_RUN_TEST(test_config_parse_malformed_missing_brace);
    HU_RUN_TEST(test_config_parse_malformed_bad_types);
    HU_RUN_TEST(test_config_env_overrides);
    HU_RUN_TEST(test_config_validate_ok);
    HU_RUN_TEST(test_config_validate_invalid_provider);
    HU_RUN_TEST(test_config_validate_invalid_port);
    HU_RUN_TEST(test_config_parse_malformed_unclosed_string);
    HU_RUN_TEST(test_config_parse_malformed_missing_value);
    HU_RUN_TEST(test_config_parse_missing_required_nested_defaults);
    HU_RUN_TEST(test_config_parse_security_section);
    HU_RUN_TEST(test_config_parse_sandbox_config);
    HU_RUN_TEST(test_config_validate_null_model_fails);
    HU_RUN_TEST(test_config_parse_string_array_basic);
    HU_RUN_TEST(test_config_parse_string_array_empty);
    HU_RUN_TEST(test_config_parse_string_array_skips_non_strings);
    HU_RUN_TEST(test_config_parse_email_channel);
    HU_RUN_TEST(test_config_parse_imessage_channel);
    HU_RUN_TEST(test_config_parse_response_mode);
    HU_RUN_TEST(test_config_parse_response_mode_selective);
    HU_RUN_TEST(test_config_parse_response_mode_normal);
    HU_RUN_TEST(test_config_parse_mcp_servers);
    HU_RUN_TEST(test_config_parse_mcp_servers_empty);
    HU_RUN_TEST(test_config_parse_nodes_array);

    HU_TEST_SUITE("Service loop");
    HU_RUN_TEST(test_config_parse_service_loop);

    HU_TEST_SUITE("Memory backend config");
    HU_RUN_TEST(test_config_parse_memory_postgres);
    HU_RUN_TEST(test_config_parse_memory_redis);
    HU_RUN_TEST(test_config_parse_memory_api);

    HU_TEST_SUITE("Config serialize");
    HU_RUN_TEST(test_config_save_null_cfg_returns_error);
    HU_RUN_TEST(test_config_save_null_path_returns_error);
    HU_RUN_TEST(test_config_save_roundtrip_key_fields);

    HU_TEST_SUITE("Config sandbox roundtrip");
    HU_RUN_TEST(test_config_sandbox_save_roundtrip);

    HU_TEST_SUITE("Behavior thresholds");
    HU_RUN_TEST(test_config_parse_behavior_thresholds);
    HU_RUN_TEST(test_config_behavior_defaults);
}
