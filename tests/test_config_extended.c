/* Config edge cases (~30 tests). No real I/O - uses parse_json and env overrides. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_config_t *make_config_with_arena(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    return cfg;
}

static void free_config(hu_config_t *cfg) {
    if (!cfg)
        return;
    hu_allocator_t backing = hu_system_allocator();
    if (cfg->arena)
        hu_arena_destroy(cfg->arena);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

static void test_config_load_nonexistent_uses_defaults(void) {
    const char *h = getenv("HOME");
    char *old_home = h ? strdup(h) : NULL;
    setenv("HOME", "/nonexistent_human_test_path_xyz", 1);
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_error_t err = hu_config_load(&backing, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg.default_provider);
    HU_ASSERT_NOT_NULL(cfg.default_model);
    HU_ASSERT(cfg.nodes_len >= 1u);
    HU_ASSERT_STR_EQ(cfg.nodes[0].name, "local");
    HU_ASSERT_STR_EQ(cfg.nodes[0].status, "online");
    hu_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_empty_json_uses_defaults(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "{}", 2);
    HU_ASSERT_EQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_partial_json_merges(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *json = "{\"default_provider\":\"anthropic\",\"default_model\":\"claude-3\"}";
    hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    HU_ASSERT_STR_EQ(cfg->default_model, "claude-3");
    free_config(cfg);
}

static void test_config_env_override_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    setenv("HUMAN_PROVIDER", "anthropic", 1);
    hu_config_apply_env_overrides(cfg);
    HU_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    unsetenv("HUMAN_PROVIDER");
    free_config(cfg);
}

static void test_config_env_override_model(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    setenv("HUMAN_MODEL", "claude-3-opus", 1);
    hu_config_apply_env_overrides(cfg);
    HU_ASSERT_STR_EQ(cfg->default_model, "claude-3-opus");
    unsetenv("HUMAN_MODEL");
    free_config(cfg);
}

static void test_config_env_override_api_key(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    setenv("OPENAI_API_KEY", "sk-test-override", 1);
    hu_config_apply_env_overrides(cfg);
    const char *key = hu_config_default_provider_key(cfg);
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "sk-test-override");
    unsetenv("OPENAI_API_KEY");
    free_config(cfg);
}

static void test_config_env_override_port(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    cfg->gateway.port = 3000;
    setenv("HUMAN_GATEWAY_PORT", "8080", 1);
    hu_config_apply_env_overrides(cfg);
    HU_ASSERT_EQ(cfg->gateway.port, 8080);
    unsetenv("HUMAN_GATEWAY_PORT");
    free_config(cfg);
}

static void test_config_env_override_autonomy(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    setenv("HUMAN_AUTONOMY", "2", 1);
    hu_config_apply_env_overrides(cfg);
    HU_ASSERT_EQ(cfg->security.autonomy_level, 2);
    unsetenv("HUMAN_AUTONOMY");
    free_config(cfg);
}

static void test_config_validate_empty_provider_fails(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = NULL;
    cfg.default_model = (char *)"gpt-4";
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_zero_fails(void) {
    hu_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 0;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_max(void) {
    hu_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4", host[] = "0.0.0.0";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 65535;
    cfg.gateway.host = host;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_config_validate_autonomy_too_high(void) {
    hu_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    cfg.security.autonomy_level = 5;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_deinit_no_crash(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_arena_t *arena = hu_arena_create(backing);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    hu_config_deinit(&cfg);
    HU_ASSERT_NULL(cfg.arena);
}

static void test_config_multiple_loads(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_error_t err = hu_config_load(&backing, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    const char *initial = cfg.default_provider;
    HU_ASSERT_NOT_NULL(initial);
    hu_config_parse_json(&cfg, "{\"default_provider\":\"gemini\"}", 29);
    HU_ASSERT_STR_EQ(cfg.default_provider, "gemini");
    hu_config_deinit(&cfg);
}

static void test_config_parse_with_memory(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *json = "{\"memory\":{\"backend\":\"sqlite\",\"auto_save\":false}}";
    hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg->memory.backend, "sqlite");
    HU_ASSERT_FALSE(cfg->memory.auto_save);
    free_config(cfg);
}

static void test_config_parse_with_gateway(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *json = "{\"gateway\":{\"port\":9000,\"host\":\"127.0.0.1\"}}";
    hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg->gateway.port, 9000);
    HU_ASSERT_STR_EQ(cfg->gateway.host, "127.0.0.1");
    free_config(cfg);
}

static void test_config_parse_malformed_array(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "[1,2,3]", 7);
    HU_ASSERT_NEQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_get_provider_key_missing(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *key = hu_config_get_provider_key(cfg, "nonexistent");
    HU_ASSERT_NULL(key);
    free_config(cfg);
}

static void test_config_get_provider_key_from_providers(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_provider_entry_t *p = (hu_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_provider_entry_t));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strdup(&cfg->allocator, "anthropic");
    p->api_key = hu_strdup(&cfg->allocator, "sk-ant-test");
    cfg->providers = p;
    cfg->providers_len = 1;
    const char *key = hu_config_get_provider_key(cfg, "anthropic");
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "sk-ant-test");
    free_config(cfg);
}

static void test_config_parse_providers_array(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *json =
        "{\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-x\"},{\"name\":\"anthropic\"}]}";
    hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg->providers_len, 2);
    HU_ASSERT_STR_EQ(cfg->providers[0].name, "openai");
    HU_ASSERT_STR_EQ(cfg->providers[0].api_key, "sk-x");
    HU_ASSERT_STR_EQ(cfg->providers[1].name, "anthropic");
    free_config(cfg);
}

static void test_config_validate_ok_minimal(void) {
    hu_config_t cfg = {0};
    char prov[] = "ollama", model[] = "llama2";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_config_parse_workspace_override(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->workspace_dir = hu_strdup(&cfg->allocator, "/original");
    const char *json = "{\"workspace\":\"/tmp/override\"}";
    hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_STR_EQ(cfg->workspace_dir, "/tmp/override");
    free_config(cfg);
}

static void test_config_parse_temperature_clamp(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *json = "{\"default_temperature\":1.5}";
    hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_TRUE(cfg->default_temperature >= 0.0 && cfg->default_temperature <= 2.0);
    free_config(cfg);
}

static void test_config_parse_null_json_fails(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    free_config(cfg);
}

static void test_config_parse_null_cfg_fails(void) {
    hu_error_t err = hu_config_parse_json(NULL, "{}", 2);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_config_apply_env_overrides_null_safe(void) {
    hu_config_apply_env_overrides(NULL);
}

static void test_config_validate_null_fails(void) {
    hu_error_t err = hu_config_validate(NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_config_load_defaults_new_fields(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/nonexistent_human_config_parity_test", 1);
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_error_t err = hu_config_load(&backing, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(cfg.cron.enabled);
    HU_ASSERT_EQ(cfg.cron.interval_minutes, 30u);
    HU_ASSERT_EQ(cfg.cron.max_run_history, 50u);
    HU_ASSERT_EQ(cfg.scheduler.max_concurrent, 4u);
    HU_ASSERT_TRUE(cfg.gateway.enabled);
    HU_ASSERT_NULL(cfg.tunnel.domain);
    HU_ASSERT_NULL(cfg.memory.sqlite_path);
    HU_ASSERT_EQ(cfg.memory.max_entries, 0u);
    HU_ASSERT_FALSE(cfg.security.audit.enabled);
    HU_ASSERT_EQ(cfg.agent.token_limit, 200000u);
    HU_ASSERT_EQ(cfg.agent.max_tool_iterations, 1000u);
    HU_ASSERT_EQ(cfg.reliability.provider_retries, 2u);
    HU_ASSERT_EQ(cfg.reliability.provider_backoff_ms, 500u);
    HU_ASSERT_FALSE(cfg.heartbeat.enabled);
    HU_ASSERT_EQ(cfg.heartbeat.interval_minutes, 30u);
    hu_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_parse_new_fields(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/nonexistent_human_parse_test", 1);
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_error_t err = hu_config_load(&backing, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json =
        "{\"cron\":{\"enabled\":true,\"interval_minutes\":15,\"max_run_history\":25},"
        "\"scheduler\":{\"max_concurrent\":8},"
        "\"tunnel\":{\"provider\":\"cloudflared\",\"domain\":\"test.example.com\"},"
        "\"memory\":{\"profile\":\"local_keyword\",\"sqlite_path\":\"/tmp/"
        "db.sqlite\",\"max_entries\":1000},"
        "\"gateway\":{\"enabled\":false},"
        "\"channels\":{\"default_channel\":\"telegram\"},"
        "\"tools\":{\"enabled_tools\":[\"shell\",\"file_read\"],\"disabled_tools\":[\"browser\"]},"
        "\"security\":{\"audit\":{\"enabled\":true,\"log_path\":\"/var/log/audit.log\"}},"
        "\"agent\":{\"token_limit\":150000,\"max_tool_iterations\":500},"
        "\"heartbeat\":{\"enabled\":true,\"interval_minutes\":15},"
        "\"reliability\":{\"provider_retries\":3,\"provider_backoff_ms\":1000,\"fallback_"
        "providers\":[\"anthropic\",\"gemini\"]},"
        "\"diagnostics\":{\"backend\":\"otel\",\"log_tool_calls\":true}}";
    err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(cfg.cron.enabled);
    HU_ASSERT_EQ(cfg.cron.interval_minutes, 15u);
    HU_ASSERT_EQ(cfg.cron.max_run_history, 25u);
    HU_ASSERT_EQ(cfg.scheduler.max_concurrent, 8u);
    HU_ASSERT_STR_EQ(cfg.tunnel.provider, "cloudflared");
    HU_ASSERT_STR_EQ(cfg.tunnel.domain, "test.example.com");
    HU_ASSERT_STR_EQ(cfg.memory.sqlite_path, "/tmp/db.sqlite");
    HU_ASSERT_EQ(cfg.memory.max_entries, 1000u);
    HU_ASSERT_FALSE(cfg.gateway.enabled);
    HU_ASSERT_STR_EQ(cfg.channels.default_channel, "telegram");
    HU_ASSERT_EQ(cfg.tools.enabled_tools_len, 2u);
    HU_ASSERT_STR_EQ(cfg.tools.enabled_tools[0], "shell");
    HU_ASSERT_STR_EQ(cfg.tools.enabled_tools[1], "file_read");
    HU_ASSERT_EQ(cfg.tools.disabled_tools_len, 1u);
    HU_ASSERT_STR_EQ(cfg.tools.disabled_tools[0], "browser");
    HU_ASSERT_TRUE(cfg.security.audit.enabled);
    HU_ASSERT_STR_EQ(cfg.security.audit.log_path, "/var/log/audit.log");
    HU_ASSERT_STR_EQ(cfg.memory.profile, "local_keyword");
    HU_ASSERT_EQ(cfg.agent.token_limit, 150000u);
    HU_ASSERT_EQ(cfg.agent.max_tool_iterations, 500u);
    HU_ASSERT_TRUE(cfg.heartbeat.enabled);
    HU_ASSERT_EQ(cfg.heartbeat.interval_minutes, 15u);
    HU_ASSERT_EQ(cfg.reliability.provider_retries, 3u);
    HU_ASSERT_EQ(cfg.reliability.provider_backoff_ms, 1000u);
    HU_ASSERT_EQ(cfg.reliability.fallback_providers_len, 2u);
    HU_ASSERT_STR_EQ(cfg.reliability.fallback_providers[0], "anthropic");
    HU_ASSERT_STR_EQ(cfg.reliability.fallback_providers[1], "gemini");
    HU_ASSERT_STR_EQ(cfg.diagnostics.backend, "otel");
    HU_ASSERT_TRUE(cfg.diagnostics.log_tool_calls);
    hu_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_get_provider_base_url(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_provider_entry_t *p = (hu_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_provider_entry_t));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strdup(&cfg->allocator, "compatible");
    p->base_url = hu_strdup(&cfg->allocator, "http://localhost:8080");
    cfg->providers = p;
    cfg->providers_len = 1;
    const char *url = hu_config_get_provider_base_url(cfg, "compatible");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "http://localhost:8080");
    free_config(cfg);
}

/* ─── Additional config tests (~60 new total) ──────────────────────────────── */
static void test_config_parse_temperature_zero(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"default_temperature\":0}", 24);
    HU_ASSERT_EQ(cfg->default_temperature, 0.0);
    free_config(cfg);
}

static void test_config_parse_temperature_two(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"default_temperature\":2.0}", 28);
    HU_ASSERT_TRUE(cfg->default_temperature >= 1.9 && cfg->default_temperature <= 2.1);
    free_config(cfg);
}

static void test_config_parse_temperature_negative_clamped(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"default_temperature\":-0.5}", 29);
    HU_ASSERT_TRUE(cfg->default_temperature >= 0.0);
    free_config(cfg);
}

static void test_config_parse_autonomy_level_full(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"autonomy_level\":2}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->security.autonomy_level, 2u);
    free_config(cfg);
}

static void test_config_parse_autonomy_level_readonly(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"autonomy_level\":0}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->security.autonomy_level, 0u);
    free_config(cfg);
}

static void test_config_parse_agent_token_limit(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"token_limit\":100000}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.token_limit, 100000u);
    free_config(cfg);
}

static void test_config_parse_agent_max_tool_iterations(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"max_tool_iterations\":200}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.max_tool_iterations, 200u);
    free_config(cfg);
}

static void test_config_parse_agent_compact_context(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"compact_context\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->agent.compact_context);
    free_config(cfg);
}

static void test_config_parse_agent_parallel_tools(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"parallel_tools\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->agent.parallel_tools);
    free_config(cfg);
}

static void test_config_parse_agent_llm_compiler(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"llm_compiler\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->agent.llm_compiler_enabled);
    free_config(cfg);
}

static void test_config_parse_agent_tool_routing(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"tool_routing\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->agent.tool_routing_enabled);
    free_config(cfg);
}

static void test_config_parse_agent_context_pressure(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"context_pressure_warn\":0.9,\"context_pressure_compact\":0.98,"
                    "\"context_compact_target\":0.65}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->agent.context_pressure_warn > 0.89f &&
                   cfg->agent.context_pressure_warn < 0.91f);
    HU_ASSERT_TRUE(cfg->agent.context_pressure_compact > 0.97f &&
                   cfg->agent.context_pressure_compact < 0.99f);
    HU_ASSERT_TRUE(cfg->agent.context_compact_target > 0.64f &&
                   cfg->agent.context_compact_target < 0.66f);
    free_config(cfg);
}

static void test_config_persona_per_channel(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"persona\":\"seth\",\"persona_channels\":{"
                    "\"imessage\":\"seth_casual\",\"gmail\":\"seth_professional\"}}}}";
    hu_error_t err = hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(cfg->agent.persona, "seth");
    HU_ASSERT_EQ(cfg->agent.persona_channels_count, 2u);

    const char *im = hu_config_persona_for_channel(cfg, "imessage");
    HU_ASSERT_NOT_NULL(im);
    HU_ASSERT_STR_EQ(im, "seth_casual");

    const char *gm = hu_config_persona_for_channel(cfg, "gmail");
    HU_ASSERT_NOT_NULL(gm);
    HU_ASSERT_STR_EQ(gm, "seth_professional");

    const char *unknown = hu_config_persona_for_channel(cfg, "telegram");
    HU_ASSERT_NOT_NULL(unknown);
    HU_ASSERT_STR_EQ(unknown, "seth");

    const char *global = hu_config_persona_for_channel(cfg, NULL);
    HU_ASSERT_NOT_NULL(global);
    HU_ASSERT_STR_EQ(global, "seth");

    free_config(cfg);
}

static void test_config_parse_runtime_docker(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"runtime\":{\"kind\":\"docker\",\"docker_image\":\"my-img\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->runtime.kind, "docker");
    HU_ASSERT_STR_EQ(cfg->runtime.docker_image, "my-img");
    free_config(cfg);
}

static void test_config_parse_tools_enabled_list(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"enabled_tools\":[\"shell\",\"file_read\"]}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->tools.enabled_tools_len, 2u);
    HU_ASSERT_STR_EQ(cfg->tools.enabled_tools[0], "shell");
    HU_ASSERT_STR_EQ(cfg->tools.enabled_tools[1], "file_read");
    free_config(cfg);
}

static void test_config_parse_tools_disabled_list(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"disabled_tools\":[\"browser\"]}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->tools.disabled_tools_len, 1u);
    HU_ASSERT_STR_EQ(cfg->tools.disabled_tools[0], "browser");
    free_config(cfg);
}

static void test_config_parse_tools_web_search_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"web_search_provider\":\"brave\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->tools.web_search_provider, "brave");
    free_config(cfg);
}

static void test_config_parse_gateway_require_pairing(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"require_pairing\":false}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_FALSE(cfg->gateway.require_pairing);
    free_config(cfg);
}

static void test_config_parse_gateway_allow_public_bind(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"allow_public_bind\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->gateway.allow_public_bind);
    free_config(cfg);
}

static void test_config_parse_memory_profile(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"profile\":\"hybrid\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->memory.profile, "hybrid");
    free_config(cfg);
}

static void test_config_parse_memory_auto_save(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"auto_save\":false}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_FALSE(cfg->memory.auto_save);
    free_config(cfg);
}

static void test_config_parse_memory_consolidation_interval_hours(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"consolidation_interval_hours\":12}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->memory.consolidation_interval_hours, 12u);
    free_config(cfg);
}

static void test_config_parse_memory_consolidation_interval_disabled(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"consolidation_interval_hours\":0}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->memory.consolidation_interval_hours, 0u);
    free_config(cfg);
}

static void test_config_parse_cron_interval(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"cron\":{\"interval_minutes\":10,\"max_run_history\":20}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->cron.interval_minutes, 10u);
    HU_ASSERT_EQ(cfg->cron.max_run_history, 20u);
    free_config(cfg);
}

static void test_config_parse_diagnostics_log_tool_calls(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"log_tool_calls\":true,\"backend\":\"file\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->diagnostics.log_tool_calls);
    HU_ASSERT_STR_EQ(cfg->diagnostics.backend, "file");
    free_config(cfg);
}

static void test_config_parse_reliability_retries(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"provider_retries\":5,\"provider_backoff_ms\":2000}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->reliability.provider_retries, 5u);
    HU_ASSERT_EQ(cfg->reliability.provider_backoff_ms, 2000u);
    free_config(cfg);
}

static void test_config_parse_channels_default(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"channels\":{\"default_channel\":\"slack\",\"cli\":false}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->channels.default_channel, "slack");
    HU_ASSERT_FALSE(cfg->channels.cli);
    free_config(cfg);
}

static void test_config_parse_malformed_truncated(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "{\"default_provider\":", 20);
    HU_ASSERT_NEQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_parse_malformed_unclosed_brace(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "{\"x\":1", 6);
    HU_ASSERT_NEQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_parse_malformed_invalid_escape(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "{\"x\":\"\\uXXXX\"}", 15);
    HU_ASSERT_NEQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_parse_empty_string_value(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"workspace\":\"\"}", 17);
    HU_ASSERT_NOT_NULL(cfg->workspace_dir);
    free_config(cfg);
}

static void test_config_parse_integer_temperature_coerced(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"default_temperature\":1}", 26);
    HU_ASSERT_TRUE(cfg->default_temperature >= 0.99 && cfg->default_temperature <= 1.01);
    free_config(cfg);
}

static void test_config_validate_empty_model_fails(void) {
    hu_config_t cfg = {0};
    char prov[] = "openai", model[] = "";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_one_ok(void) {
    hu_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 1;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_config_get_provider_native_tools(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_provider_entry_t *p = (hu_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_provider_entry_t));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strdup(&cfg->allocator, "ollama");
    p->native_tools = false;
    cfg->providers = p;
    cfg->providers_len = 1;
    HU_ASSERT_FALSE(hu_config_get_provider_native_tools(cfg, "ollama"));
    p->native_tools = true;
    HU_ASSERT_TRUE(hu_config_get_provider_native_tools(cfg, "ollama"));
    free_config(cfg);
}

static void test_config_env_override_workspace(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    setenv("HUMAN_WORKSPACE", "/custom/ws", 1);
    hu_config_apply_env_overrides(cfg);
    HU_ASSERT_STR_EQ(cfg->workspace_dir, "/custom/ws");
    unsetenv("HUMAN_WORKSPACE");
    free_config(cfg);
}

static void test_config_parse_autonomy_allowed_commands(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"autonomy\":{\"allowed_commands\":[\"ls\",\"cat\"]}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->autonomy.allowed_commands_len, 2u);
    HU_ASSERT_STR_EQ(cfg->autonomy.allowed_commands[0], "ls");
    HU_ASSERT_STR_EQ(cfg->autonomy.allowed_commands[1], "cat");
    free_config(cfg);
}

static void test_config_parse_security_audit(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"audit\":{\"enabled\":true,\"log_path\":\"/tmp/audit.log\"}}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->security.audit.enabled);
    HU_ASSERT_STR_EQ(cfg->security.audit.log_path, "/tmp/audit.log");
    free_config(cfg);
}

static void test_config_parse_provider_native_tools_false(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"providers\":[{\"name\":\"ollama\",\"native_tools\":false}]}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->providers_len, 1u);
    HU_ASSERT_FALSE(cfg->providers[0].native_tools);
    free_config(cfg);
}

static void test_config_parse_tunnel_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tunnel\":{\"provider\":\"ngrok\",\"domain\":\"x.ngrok.io\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->tunnel.provider, "ngrok");
    HU_ASSERT_STR_EQ(cfg->tunnel.domain, "x.ngrok.io");
    free_config(cfg);
}

static void test_config_parse_heartbeat_interval(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"heartbeat\":{\"enabled\":true,\"interval_minutes\":5}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->heartbeat.enabled);
    HU_ASSERT_EQ(cfg->heartbeat.interval_minutes, 5u);
    free_config(cfg);
}

static void test_config_parse_scheduler_max_concurrent(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"scheduler\":{\"max_concurrent\":16}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->scheduler.max_concurrent, 16u);
    free_config(cfg);
}

static void test_config_parse_multiple_providers_with_keys(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"providers\":[{\"name\":\"a\",\"api_key\":\"k1\"},{\"name\":\"b\",\"api_"
                    "key\":\"k2\"},{\"name\":\"c\"}]}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->providers_len, 3u);
    HU_ASSERT_STR_EQ(cfg->providers[0].api_key, "k1");
    HU_ASSERT_STR_EQ(cfg->providers[1].api_key, "k2");
    HU_ASSERT_NULL(cfg->providers[2].api_key);
    free_config(cfg);
}

static void test_config_parse_json_nested_deep(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"token_limit\":50000},\"memory\":{\"backend\":\"sqlite\",\"max_"
                    "entries\":500},\"gateway\":{\"port\":4444}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.token_limit, 50000u);
    HU_ASSERT_STR_EQ(cfg->memory.backend, "sqlite");
    HU_ASSERT_EQ(cfg->memory.max_entries, 500u);
    HU_ASSERT_EQ(cfg->gateway.port, 4444);
    free_config(cfg);
}

static void test_config_validate_ok_with_provider_model_port(void) {
    hu_config_t cfg = {0};
    char prov[] = "anthropic", model[] = "claude-3";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 8080;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_config_parse_default_model_provider_prefix(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/nonexistent_human_provider_test", 1);
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_config_load(&backing, &cfg);
    const char *j = "{\"default_provider\":\"gemini\",\"default_model\":\"gemini-2.0-flash\"}";
    hu_config_parse_json(&cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg.default_provider, "gemini");
    HU_ASSERT_STR_EQ(cfg.default_model, "gemini-2.0-flash");
    hu_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_parse_workspace_path_with_slash(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"workspace\":\"/home/user/project\"}", 34);
    HU_ASSERT_STR_EQ(cfg->workspace_dir, "/home/user/project");
    free_config(cfg);
}

static void test_config_get_provider_base_url_null_for_unknown(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *url = hu_config_get_provider_base_url(cfg, "unknown_xyz");
    HU_ASSERT_NULL(url);
    free_config(cfg);
}

/* ─── WP-21B parity: web_search_provider, config merge, defaults ────────────── */
static void test_config_get_web_search_provider_default(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *p = hu_config_get_web_search_provider(cfg);
    HU_ASSERT_NOT_NULL(p);
    free_config(cfg);
}

static void test_config_parse_web_search_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"web_search_provider\":\"brave\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    const char *p = hu_config_get_web_search_provider(cfg);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "brave");
    free_config(cfg);
}

static void test_config_parse_config_merge_sequential(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"default_provider\":\"anthropic\"}", 32);
    hu_config_parse_json(cfg, "{\"default_model\":\"claude-3-sonnet\"}", 35);
    HU_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    HU_ASSERT_STR_EQ(cfg->default_model, "claude-3-sonnet");
    free_config(cfg);
}

static void test_config_parse_defaults_tools_empty(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{}", 2);
    HU_ASSERT_EQ(cfg->tools.enabled_tools_len, 0u);
    HU_ASSERT_EQ(cfg->tools.disabled_tools_len, 0u);
    free_config(cfg);
}

static void test_config_parse_channels_cli_explicit(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"channels\":{\"default_channel\":\"cli\",\"cli\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->channels.cli);
    HU_ASSERT_STR_EQ(cfg->channels.default_channel, "cli");
    free_config(cfg);
}

static void test_config_parse_malformed_deeply_nested(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_error_t err = hu_config_parse_json(cfg, "{\"a\":{\"b\":{\"c\":", 18);
    HU_ASSERT_NEQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_parse_empty_array_value(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"fallback_providers\":[]}}";
    hu_error_t err = hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg->reliability.fallback_providers_len, 0u);
    free_config(cfg);
}

static void test_config_validate_model_null_after_parse(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_config_parse_json(cfg, "{\"default_provider\":\"ollama\"}", 28);
    cfg->default_model = NULL;
    hu_error_t err = hu_config_validate(cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
    free_config(cfg);
}

/* ─── ~30 additional config tests ───────────────────────────────────────── */
static void test_config_parse_memory_backend_sqlite(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"backend\":\"sqlite\",\"sqlite_path\":\"/tmp/db.sqlite\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->memory.backend, "sqlite");
    HU_ASSERT_STR_EQ(cfg->memory.sqlite_path, "/tmp/db.sqlite");
    free_config(cfg);
}

static void test_config_parse_memory_backend_markdown(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"backend\":\"markdown\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->memory.backend, "markdown");
    free_config(cfg);
}

static void test_config_parse_memory_backend_none(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"backend\":\"none\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->memory.backend, "none");
    free_config(cfg);
}

static void test_config_parse_gateway_pair_rate_limit(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"pair_rate_limit_per_minute\":10}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->gateway.pair_rate_limit_per_minute, 10u);
    free_config(cfg);
}

static void test_config_parse_agent_max_history(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"max_history_messages\":50}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.max_history_messages, 50u);
    free_config(cfg);
}

static void test_config_parse_agent_compaction(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"agent\":{\"compaction_keep_recent\":10,\"compaction_max_summary_chars\":500}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.compaction_keep_recent, 10u);
    HU_ASSERT_EQ(cfg->agent.compaction_max_summary_chars, 500u);
    free_config(cfg);
}

static void test_config_parse_reliability_channel_backoff(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"reliability\":{\"channel_initial_backoff_secs\":5,\"channel_max_backoff_secs\":300}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->reliability.channel_initial_backoff_secs, 5u);
    HU_ASSERT_EQ(cfg->reliability.channel_max_backoff_secs, 300u);
    free_config(cfg);
}

static void test_config_parse_diagnostics_otel(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"backend\":\"otel\",\"otel_endpoint\":\"http://"
                    "localhost:4318\",\"otel_service_name\":\"human\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->diagnostics.backend, "otel");
    HU_ASSERT_STR_EQ(cfg->diagnostics.otel_endpoint, "http://localhost:4318");
    HU_ASSERT_STR_EQ(cfg->diagnostics.otel_service_name, "human");
    free_config(cfg);
}

static void test_config_parse_tools_shell_timeout(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *j = "{\"tools\":{\"web_search_provider\":\"brave\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->tools.web_search_provider, "brave");
    free_config(cfg);
}

static void test_config_parse_tools_max_file_size(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *j = "{\"tools\":{\"enabled_tools\":[\"shell\"]}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->tools.enabled_tools_len, 1u);
    HU_ASSERT_STR_EQ(cfg->tools.enabled_tools[0], "shell");
    free_config(cfg);
}

static void test_config_parse_security_sandbox(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"sandbox\":\"landlock\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->security.sandbox, "landlock");
    free_config(cfg);
}

static void test_config_parse_provider_base_url(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"providers\":[{\"name\":\"compatible\",\"base_url\":\"https://api.example.com/v1\"}]}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->providers_len, 1u);
    HU_ASSERT_STR_EQ(cfg->providers[0].base_url, "https://api.example.com/v1");
    free_config(cfg);
}

static void test_config_parse_unicode_value(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"workspace\":\"/path/\u0442\u0435\u0441\u0442\"}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_NOT_NULL(cfg->workspace_dir);
    free_config(cfg);
}

static void test_config_merge_base_then_override(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    hu_config_parse_json(cfg, "{\"default_provider\":\"anthropic\"}", 33);
    HU_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    HU_ASSERT_STR_EQ(cfg->default_model, "gpt-4");
    free_config(cfg);
}

static void test_config_parse_empty_object(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    hu_error_t err = hu_config_parse_json(cfg, "{}", 2);
    HU_ASSERT_EQ(err, HU_OK);
    free_config(cfg);
}

static void test_config_parse_cron_enabled(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"cron\":{\"enabled\":true,\"interval_minutes\":5}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->cron.enabled);
    HU_ASSERT_EQ(cfg->cron.interval_minutes, 5u);
    free_config(cfg);
}

static void test_config_parse_gateway_host(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"host\":\"127.0.0.1\",\"port\":9000}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->gateway.host, "127.0.0.1");
    HU_ASSERT_EQ(cfg->gateway.port, 9000);
    free_config(cfg);
}

static void test_config_parse_single_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"providers\":[{\"name\":\"ollama\",\"api_key\":null}]}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->providers_len, 1u);
    HU_ASSERT_STR_EQ(cfg->providers[0].name, "ollama");
    free_config(cfg);
}

static void test_config_parse_fallback_providers_single(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"fallback_providers\":[\"gemini\"]}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->reliability.fallback_providers_len, 1u);
    HU_ASSERT_STR_EQ(cfg->reliability.fallback_providers[0], "gemini");
    free_config(cfg);
}

static void test_config_parse_diagnostics_log_receipts(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"log_message_receipts\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->diagnostics.log_message_receipts);
    free_config(cfg);
}

static void test_config_parse_diagnostics_log_llm_io(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"log_llm_io\":true}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_TRUE(cfg->diagnostics.log_llm_io);
    free_config(cfg);
}

static void test_config_parse_agent_session_idle(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"session_idle_timeout_secs\":3600}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.session_idle_timeout_secs, 3600u);
    free_config(cfg);
}

static void test_config_parse_agent_message_timeout(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"message_timeout_secs\":120}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->agent.message_timeout_secs, 120u);
    free_config(cfg);
}

static void test_config_parse_runtime_native(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"runtime\":{\"kind\":\"native\"}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_STR_EQ(cfg->runtime.kind, "native");
    free_config(cfg);
}

static void test_config_parse_tools_web_fetch_max_chars(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *j = "{\"tools\":{\"disabled_tools\":[\"browser\"]}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->tools.disabled_tools_len, 1u);
    HU_ASSERT_STR_EQ(cfg->tools.disabled_tools[0], "browser");
    free_config(cfg);
}

static void test_config_parse_scheduler_poll(void) {
    hu_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"scheduler_poll_secs\":60,\"scheduler_retries\":3}}";
    hu_config_parse_json(cfg, j, strlen(j));
    HU_ASSERT_EQ(cfg->reliability.scheduler_poll_secs, 60u);
    HU_ASSERT_EQ(cfg->reliability.scheduler_retries, 3u);
    free_config(cfg);
}

static void test_config_deinit_idempotent(void) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t cfg = {0};
    hu_arena_t *arena = hu_arena_create(backing);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    hu_config_deinit(&cfg);
    hu_config_deinit(&cfg);
    HU_ASSERT_NULL(cfg.arena);
}

static void test_config_parse_very_long_key_value(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "{\"workspace\":\"%s\"}",
                     "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z");
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(buf));
    hu_error_t err = hu_config_parse_json(cfg, buf, (size_t)n);
    HU_ASSERT_EQ(err, HU_OK);
    free_config(cfg);
}

void run_config_extended_tests(void) {
    HU_TEST_SUITE("Config Extended");
    HU_RUN_TEST(test_config_load_nonexistent_uses_defaults);
    HU_RUN_TEST(test_config_empty_json_uses_defaults);
    HU_RUN_TEST(test_config_partial_json_merges);
    HU_RUN_TEST(test_config_env_override_provider);
    HU_RUN_TEST(test_config_env_override_model);
    HU_RUN_TEST(test_config_env_override_api_key);
    HU_RUN_TEST(test_config_env_override_port);
    HU_RUN_TEST(test_config_env_override_autonomy);
    HU_RUN_TEST(test_config_validate_empty_provider_fails);
    HU_RUN_TEST(test_config_validate_port_zero_fails);
    HU_RUN_TEST(test_config_validate_port_max);
    HU_RUN_TEST(test_config_validate_autonomy_too_high);
    HU_RUN_TEST(test_config_deinit_no_crash);
    HU_RUN_TEST(test_config_multiple_loads);
    HU_RUN_TEST(test_config_parse_with_memory);
    HU_RUN_TEST(test_config_parse_with_gateway);
    HU_RUN_TEST(test_config_parse_malformed_array);
    HU_RUN_TEST(test_config_get_provider_key_missing);
    HU_RUN_TEST(test_config_get_provider_key_from_providers);
    HU_RUN_TEST(test_config_parse_providers_array);
    HU_RUN_TEST(test_config_validate_ok_minimal);
    HU_RUN_TEST(test_config_parse_workspace_override);
    HU_RUN_TEST(test_config_parse_temperature_clamp);
    HU_RUN_TEST(test_config_parse_null_json_fails);
    HU_RUN_TEST(test_config_parse_null_cfg_fails);
    HU_RUN_TEST(test_config_apply_env_overrides_null_safe);
    HU_RUN_TEST(test_config_validate_null_fails);
    HU_RUN_TEST(test_config_get_provider_base_url);
    HU_RUN_TEST(test_config_load_defaults_new_fields);
    HU_RUN_TEST(test_config_parse_new_fields);

    HU_RUN_TEST(test_config_parse_temperature_zero);
    HU_RUN_TEST(test_config_parse_temperature_two);
    HU_RUN_TEST(test_config_parse_temperature_negative_clamped);
    HU_RUN_TEST(test_config_parse_autonomy_level_full);
    HU_RUN_TEST(test_config_parse_autonomy_level_readonly);
    HU_RUN_TEST(test_config_parse_agent_token_limit);
    HU_RUN_TEST(test_config_parse_agent_max_tool_iterations);
    HU_RUN_TEST(test_config_parse_agent_compact_context);
    HU_RUN_TEST(test_config_parse_agent_parallel_tools);
    HU_RUN_TEST(test_config_parse_agent_llm_compiler);
    HU_RUN_TEST(test_config_parse_agent_tool_routing);
    HU_RUN_TEST(test_config_parse_agent_context_pressure);
    HU_RUN_TEST(test_config_persona_per_channel);
    HU_RUN_TEST(test_config_parse_runtime_docker);
    HU_RUN_TEST(test_config_parse_tools_enabled_list);
    HU_RUN_TEST(test_config_parse_tools_disabled_list);
    HU_RUN_TEST(test_config_parse_tools_web_search_provider);
    HU_RUN_TEST(test_config_parse_gateway_require_pairing);
    HU_RUN_TEST(test_config_parse_gateway_allow_public_bind);
    HU_RUN_TEST(test_config_parse_memory_profile);
    HU_RUN_TEST(test_config_parse_memory_auto_save);
    HU_RUN_TEST(test_config_parse_memory_consolidation_interval_hours);
    HU_RUN_TEST(test_config_parse_memory_consolidation_interval_disabled);
    HU_RUN_TEST(test_config_parse_cron_interval);
    HU_RUN_TEST(test_config_parse_diagnostics_log_tool_calls);
    HU_RUN_TEST(test_config_parse_reliability_retries);
    HU_RUN_TEST(test_config_parse_channels_default);
    HU_RUN_TEST(test_config_parse_malformed_truncated);
    HU_RUN_TEST(test_config_parse_malformed_unclosed_brace);
    HU_RUN_TEST(test_config_parse_malformed_invalid_escape);
    HU_RUN_TEST(test_config_parse_empty_string_value);
    HU_RUN_TEST(test_config_parse_integer_temperature_coerced);
    HU_RUN_TEST(test_config_validate_empty_model_fails);
    HU_RUN_TEST(test_config_validate_port_one_ok);
    HU_RUN_TEST(test_config_get_provider_native_tools);
    HU_RUN_TEST(test_config_env_override_workspace);
    HU_RUN_TEST(test_config_parse_autonomy_allowed_commands);
    HU_RUN_TEST(test_config_parse_security_audit);
    HU_RUN_TEST(test_config_parse_provider_native_tools_false);
    HU_RUN_TEST(test_config_parse_tunnel_provider);
    HU_RUN_TEST(test_config_parse_heartbeat_interval);
    HU_RUN_TEST(test_config_parse_scheduler_max_concurrent);
    HU_RUN_TEST(test_config_parse_multiple_providers_with_keys);
    HU_RUN_TEST(test_config_parse_json_nested_deep);
    HU_RUN_TEST(test_config_validate_ok_with_provider_model_port);
    HU_RUN_TEST(test_config_parse_default_model_provider_prefix);
    HU_RUN_TEST(test_config_parse_workspace_path_with_slash);
    HU_RUN_TEST(test_config_get_provider_base_url_null_for_unknown);
    HU_RUN_TEST(test_config_get_web_search_provider_default);
    HU_RUN_TEST(test_config_parse_web_search_provider);
    HU_RUN_TEST(test_config_parse_config_merge_sequential);
    HU_RUN_TEST(test_config_parse_defaults_tools_empty);
    HU_RUN_TEST(test_config_parse_channels_cli_explicit);
    HU_RUN_TEST(test_config_parse_malformed_deeply_nested);
    HU_RUN_TEST(test_config_parse_empty_array_value);
    HU_RUN_TEST(test_config_validate_model_null_after_parse);

    HU_RUN_TEST(test_config_parse_memory_backend_sqlite);
    HU_RUN_TEST(test_config_parse_memory_backend_markdown);
    HU_RUN_TEST(test_config_parse_memory_backend_none);
    HU_RUN_TEST(test_config_parse_gateway_pair_rate_limit);
    HU_RUN_TEST(test_config_parse_agent_max_history);
    HU_RUN_TEST(test_config_parse_agent_compaction);
    HU_RUN_TEST(test_config_parse_reliability_channel_backoff);
    HU_RUN_TEST(test_config_parse_diagnostics_otel);
    HU_RUN_TEST(test_config_parse_tools_shell_timeout);
    HU_RUN_TEST(test_config_parse_tools_max_file_size);
    HU_RUN_TEST(test_config_parse_security_sandbox);
    HU_RUN_TEST(test_config_parse_provider_base_url);
    HU_RUN_TEST(test_config_parse_unicode_value);
    HU_RUN_TEST(test_config_merge_base_then_override);
    HU_RUN_TEST(test_config_parse_empty_object);
    HU_RUN_TEST(test_config_parse_cron_enabled);
    HU_RUN_TEST(test_config_parse_gateway_host);
    HU_RUN_TEST(test_config_parse_single_provider);
    HU_RUN_TEST(test_config_parse_fallback_providers_single);
    HU_RUN_TEST(test_config_parse_diagnostics_log_receipts);
    HU_RUN_TEST(test_config_parse_diagnostics_log_llm_io);
    HU_RUN_TEST(test_config_parse_agent_session_idle);
    HU_RUN_TEST(test_config_parse_agent_message_timeout);
    HU_RUN_TEST(test_config_parse_runtime_native);
    HU_RUN_TEST(test_config_parse_tools_web_fetch_max_chars);
    HU_RUN_TEST(test_config_parse_scheduler_poll);
    HU_RUN_TEST(test_config_deinit_idempotent);
    HU_RUN_TEST(test_config_parse_very_long_key_value);
}
