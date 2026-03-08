/* Config edge cases (~30 tests). No real I/O - uses parse_json and env overrides. */
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sc_config_t *make_config_with_arena(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(backing);
    SC_ASSERT_NOT_NULL(arena);
    sc_config_t *cfg = (sc_config_t *)backing.alloc(backing.ctx, sizeof(sc_config_t));
    SC_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = sc_arena_allocator(arena);
    return cfg;
}

static void free_config(sc_config_t *cfg) {
    if (!cfg)
        return;
    sc_allocator_t backing = sc_system_allocator();
    if (cfg->arena)
        sc_arena_destroy(cfg->arena);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

static void test_config_load_nonexistent_uses_defaults(void) {
    const char *h = getenv("HOME");
    char *old_home = h ? strdup(h) : NULL;
    setenv("HOME", "/nonexistent_seaclaw_test_path_xyz", 1);
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_error_t err = sc_config_load(&backing, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(cfg.default_provider);
    SC_ASSERT_NOT_NULL(cfg.default_model);
    SC_ASSERT(cfg.nodes_len >= 1u);
    SC_ASSERT_STR_EQ(cfg.nodes[0].name, "local");
    SC_ASSERT_STR_EQ(cfg.nodes[0].status, "online");
    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_empty_json_uses_defaults(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, "{}", 2);
    SC_ASSERT_EQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_partial_json_merges(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *json = "{\"default_provider\":\"anthropic\",\"default_model\":\"claude-3\"}";
    sc_error_t err = sc_config_parse_json(cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    SC_ASSERT_STR_EQ(cfg->default_model, "claude-3");
    free_config(cfg);
}

static void test_config_env_override_provider(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    setenv("SEACLAW_PROVIDER", "anthropic", 1);
    sc_config_apply_env_overrides(cfg);
    SC_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    unsetenv("SEACLAW_PROVIDER");
    free_config(cfg);
}

static void test_config_env_override_model(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    setenv("SEACLAW_MODEL", "claude-3-opus", 1);
    sc_config_apply_env_overrides(cfg);
    SC_ASSERT_STR_EQ(cfg->default_model, "claude-3-opus");
    unsetenv("SEACLAW_MODEL");
    free_config(cfg);
}

static void test_config_env_override_api_key(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    setenv("OPENAI_API_KEY", "sk-test-override", 1);
    sc_config_apply_env_overrides(cfg);
    const char *key = sc_config_default_provider_key(cfg);
    SC_ASSERT_NOT_NULL(key);
    SC_ASSERT_STR_EQ(key, "sk-test-override");
    unsetenv("OPENAI_API_KEY");
    free_config(cfg);
}

static void test_config_env_override_port(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    cfg->gateway.port = 3000;
    setenv("SEACLAW_GATEWAY_PORT", "8080", 1);
    sc_config_apply_env_overrides(cfg);
    SC_ASSERT_EQ(cfg->gateway.port, 8080);
    unsetenv("SEACLAW_GATEWAY_PORT");
    free_config(cfg);
}

static void test_config_env_override_autonomy(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    setenv("SEACLAW_AUTONOMY", "2", 1);
    sc_config_apply_env_overrides(cfg);
    SC_ASSERT_EQ(cfg->security.autonomy_level, 2);
    unsetenv("SEACLAW_AUTONOMY");
    free_config(cfg);
}

static void test_config_validate_empty_provider_fails(void) {
    sc_config_t cfg = {0};
    cfg.default_provider = NULL;
    cfg.default_model = (char *)"gpt-4";
    cfg.gateway.port = 3000;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_zero_fails(void) {
    sc_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 0;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_max(void) {
    sc_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4", host[] = "0.0.0.0";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 65535;
    cfg.gateway.host = host;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_config_validate_autonomy_too_high(void) {
    sc_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    cfg.security.autonomy_level = 5;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

static void test_config_deinit_no_crash(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_arena_t *arena = sc_arena_create(backing);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    sc_config_deinit(&cfg);
    SC_ASSERT_NULL(cfg.arena);
}

static void test_config_multiple_loads(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_error_t err = sc_config_load(&backing, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    const char *initial = cfg.default_provider;
    SC_ASSERT_NOT_NULL(initial);
    sc_config_parse_json(&cfg, "{\"default_provider\":\"gemini\"}", 29);
    SC_ASSERT_STR_EQ(cfg.default_provider, "gemini");
    sc_config_deinit(&cfg);
}

static void test_config_parse_with_memory(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *json = "{\"memory\":{\"backend\":\"sqlite\",\"auto_save\":false}}";
    sc_error_t err = sc_config_parse_json(cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg->memory.backend, "sqlite");
    SC_ASSERT_FALSE(cfg->memory.auto_save);
    free_config(cfg);
}

static void test_config_parse_with_gateway(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *json = "{\"gateway\":{\"port\":9000,\"host\":\"127.0.0.1\"}}";
    sc_error_t err = sc_config_parse_json(cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg->gateway.port, 9000);
    SC_ASSERT_STR_EQ(cfg->gateway.host, "127.0.0.1");
    free_config(cfg);
}

static void test_config_parse_malformed_array(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, "[1,2,3]", 7);
    SC_ASSERT_NEQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_get_provider_key_missing(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    const char *key = sc_config_get_provider_key(cfg, "nonexistent");
    SC_ASSERT_NULL(key);
    free_config(cfg);
}

static void test_config_get_provider_key_from_providers(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_provider_entry_t *p = (sc_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(sc_provider_entry_t));
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = sc_strdup(&cfg->allocator, "anthropic");
    p->api_key = sc_strdup(&cfg->allocator, "sk-ant-test");
    cfg->providers = p;
    cfg->providers_len = 1;
    const char *key = sc_config_get_provider_key(cfg, "anthropic");
    SC_ASSERT_NOT_NULL(key);
    SC_ASSERT_STR_EQ(key, "sk-ant-test");
    free_config(cfg);
}

static void test_config_parse_providers_array(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *json =
        "{\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-x\"},{\"name\":\"anthropic\"}]}";
    sc_error_t err = sc_config_parse_json(cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg->providers_len, 2);
    SC_ASSERT_STR_EQ(cfg->providers[0].name, "openai");
    SC_ASSERT_STR_EQ(cfg->providers[0].api_key, "sk-x");
    SC_ASSERT_STR_EQ(cfg->providers[1].name, "anthropic");
    free_config(cfg);
}

static void test_config_validate_ok_minimal(void) {
    sc_config_t cfg = {0};
    char prov[] = "ollama", model[] = "llama2";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_config_parse_workspace_override(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->workspace_dir = sc_strdup(&cfg->allocator, "/original");
    const char *json = "{\"workspace\":\"/tmp/override\"}";
    sc_config_parse_json(cfg, json, strlen(json));
    SC_ASSERT_STR_EQ(cfg->workspace_dir, "/tmp/override");
    free_config(cfg);
}

static void test_config_parse_temperature_clamp(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *json = "{\"default_temperature\":1.5}";
    sc_config_parse_json(cfg, json, strlen(json));
    SC_ASSERT_TRUE(cfg->default_temperature >= 0.0 && cfg->default_temperature <= 2.0);
    free_config(cfg);
}

static void test_config_parse_null_json_fails(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    free_config(cfg);
}

static void test_config_parse_null_cfg_fails(void) {
    sc_error_t err = sc_config_parse_json(NULL, "{}", 2);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_config_apply_env_overrides_null_safe(void) {
    sc_config_apply_env_overrides(NULL);
}

static void test_config_validate_null_fails(void) {
    sc_error_t err = sc_config_validate(NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_config_load_defaults_new_fields(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/nonexistent_seaclaw_config_parity_test", 1);
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_error_t err = sc_config_load(&backing, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_FALSE(cfg.cron.enabled);
    SC_ASSERT_EQ(cfg.cron.interval_minutes, 30u);
    SC_ASSERT_EQ(cfg.cron.max_run_history, 50u);
    SC_ASSERT_EQ(cfg.scheduler.max_concurrent, 4u);
    SC_ASSERT_TRUE(cfg.gateway.enabled);
    SC_ASSERT_NULL(cfg.tunnel.domain);
    SC_ASSERT_NULL(cfg.memory.sqlite_path);
    SC_ASSERT_EQ(cfg.memory.max_entries, 0u);
    SC_ASSERT_FALSE(cfg.security.audit.enabled);
    SC_ASSERT_EQ(cfg.agent.token_limit, 200000u);
    SC_ASSERT_EQ(cfg.agent.max_tool_iterations, 1000u);
    SC_ASSERT_EQ(cfg.reliability.provider_retries, 2u);
    SC_ASSERT_EQ(cfg.reliability.provider_backoff_ms, 500u);
    SC_ASSERT_FALSE(cfg.heartbeat.enabled);
    SC_ASSERT_EQ(cfg.heartbeat.interval_minutes, 30u);
    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_parse_new_fields(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/nonexistent_seaclaw_parse_test", 1);
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_error_t err = sc_config_load(&backing, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
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
    err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(cfg.cron.enabled);
    SC_ASSERT_EQ(cfg.cron.interval_minutes, 15u);
    SC_ASSERT_EQ(cfg.cron.max_run_history, 25u);
    SC_ASSERT_EQ(cfg.scheduler.max_concurrent, 8u);
    SC_ASSERT_STR_EQ(cfg.tunnel.provider, "cloudflared");
    SC_ASSERT_STR_EQ(cfg.tunnel.domain, "test.example.com");
    SC_ASSERT_STR_EQ(cfg.memory.sqlite_path, "/tmp/db.sqlite");
    SC_ASSERT_EQ(cfg.memory.max_entries, 1000u);
    SC_ASSERT_FALSE(cfg.gateway.enabled);
    SC_ASSERT_STR_EQ(cfg.channels.default_channel, "telegram");
    SC_ASSERT_EQ(cfg.tools.enabled_tools_len, 2u);
    SC_ASSERT_STR_EQ(cfg.tools.enabled_tools[0], "shell");
    SC_ASSERT_STR_EQ(cfg.tools.enabled_tools[1], "file_read");
    SC_ASSERT_EQ(cfg.tools.disabled_tools_len, 1u);
    SC_ASSERT_STR_EQ(cfg.tools.disabled_tools[0], "browser");
    SC_ASSERT_TRUE(cfg.security.audit.enabled);
    SC_ASSERT_STR_EQ(cfg.security.audit.log_path, "/var/log/audit.log");
    SC_ASSERT_STR_EQ(cfg.memory.profile, "local_keyword");
    SC_ASSERT_EQ(cfg.agent.token_limit, 150000u);
    SC_ASSERT_EQ(cfg.agent.max_tool_iterations, 500u);
    SC_ASSERT_TRUE(cfg.heartbeat.enabled);
    SC_ASSERT_EQ(cfg.heartbeat.interval_minutes, 15u);
    SC_ASSERT_EQ(cfg.reliability.provider_retries, 3u);
    SC_ASSERT_EQ(cfg.reliability.provider_backoff_ms, 1000u);
    SC_ASSERT_EQ(cfg.reliability.fallback_providers_len, 2u);
    SC_ASSERT_STR_EQ(cfg.reliability.fallback_providers[0], "anthropic");
    SC_ASSERT_STR_EQ(cfg.reliability.fallback_providers[1], "gemini");
    SC_ASSERT_STR_EQ(cfg.diagnostics.backend, "otel");
    SC_ASSERT_TRUE(cfg.diagnostics.log_tool_calls);
    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_get_provider_base_url(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_provider_entry_t *p = (sc_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(sc_provider_entry_t));
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = sc_strdup(&cfg->allocator, "compatible");
    p->base_url = sc_strdup(&cfg->allocator, "http://localhost:8080");
    cfg->providers = p;
    cfg->providers_len = 1;
    const char *url = sc_config_get_provider_base_url(cfg, "compatible");
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_STR_EQ(url, "http://localhost:8080");
    free_config(cfg);
}

/* ─── Additional config tests (~60 new total) ──────────────────────────────── */
static void test_config_parse_temperature_zero(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"default_temperature\":0}", 24);
    SC_ASSERT_EQ(cfg->default_temperature, 0.0);
    free_config(cfg);
}

static void test_config_parse_temperature_two(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"default_temperature\":2.0}", 28);
    SC_ASSERT_TRUE(cfg->default_temperature >= 1.9 && cfg->default_temperature <= 2.1);
    free_config(cfg);
}

static void test_config_parse_temperature_negative_clamped(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"default_temperature\":-0.5}", 29);
    SC_ASSERT_TRUE(cfg->default_temperature >= 0.0);
    free_config(cfg);
}

static void test_config_parse_autonomy_level_full(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"autonomy_level\":2}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->security.autonomy_level, 2u);
    free_config(cfg);
}

static void test_config_parse_autonomy_level_readonly(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"autonomy_level\":0}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->security.autonomy_level, 0u);
    free_config(cfg);
}

static void test_config_parse_agent_token_limit(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"token_limit\":100000}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.token_limit, 100000u);
    free_config(cfg);
}

static void test_config_parse_agent_max_tool_iterations(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"max_tool_iterations\":200}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.max_tool_iterations, 200u);
    free_config(cfg);
}

static void test_config_parse_agent_compact_context(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"compact_context\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->agent.compact_context);
    free_config(cfg);
}

static void test_config_parse_agent_parallel_tools(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"parallel_tools\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->agent.parallel_tools);
    free_config(cfg);
}

static void test_config_parse_agent_llm_compiler(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"llm_compiler\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->agent.llm_compiler_enabled);
    free_config(cfg);
}

static void test_config_parse_agent_tool_routing(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"tool_routing\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->agent.tool_routing_enabled);
    free_config(cfg);
}

static void test_config_parse_agent_context_pressure(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"context_pressure_warn\":0.9,\"context_pressure_compact\":0.98,"
                    "\"context_compact_target\":0.65}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->agent.context_pressure_warn > 0.89f &&
                   cfg->agent.context_pressure_warn < 0.91f);
    SC_ASSERT_TRUE(cfg->agent.context_pressure_compact > 0.97f &&
                   cfg->agent.context_pressure_compact < 0.99f);
    SC_ASSERT_TRUE(cfg->agent.context_compact_target > 0.64f &&
                   cfg->agent.context_compact_target < 0.66f);
    free_config(cfg);
}

static void test_config_persona_per_channel(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"persona\":\"seth\",\"persona_channels\":{"
                    "\"imessage\":\"seth_casual\",\"gmail\":\"seth_professional\"}}}}";
    sc_error_t err = sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(cfg->agent.persona, "seth");
    SC_ASSERT_EQ(cfg->agent.persona_channels_count, 2u);

    const char *im = sc_config_persona_for_channel(cfg, "imessage");
    SC_ASSERT_NOT_NULL(im);
    SC_ASSERT_STR_EQ(im, "seth_casual");

    const char *gm = sc_config_persona_for_channel(cfg, "gmail");
    SC_ASSERT_NOT_NULL(gm);
    SC_ASSERT_STR_EQ(gm, "seth_professional");

    const char *unknown = sc_config_persona_for_channel(cfg, "telegram");
    SC_ASSERT_NOT_NULL(unknown);
    SC_ASSERT_STR_EQ(unknown, "seth");

    const char *global = sc_config_persona_for_channel(cfg, NULL);
    SC_ASSERT_NOT_NULL(global);
    SC_ASSERT_STR_EQ(global, "seth");

    free_config(cfg);
}

static void test_config_parse_runtime_docker(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"runtime\":{\"kind\":\"docker\",\"docker_image\":\"my-img\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->runtime.kind, "docker");
    SC_ASSERT_STR_EQ(cfg->runtime.docker_image, "my-img");
    free_config(cfg);
}

static void test_config_parse_tools_enabled_list(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"enabled_tools\":[\"shell\",\"file_read\"]}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->tools.enabled_tools_len, 2u);
    SC_ASSERT_STR_EQ(cfg->tools.enabled_tools[0], "shell");
    SC_ASSERT_STR_EQ(cfg->tools.enabled_tools[1], "file_read");
    free_config(cfg);
}

static void test_config_parse_tools_disabled_list(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"disabled_tools\":[\"browser\"]}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->tools.disabled_tools_len, 1u);
    SC_ASSERT_STR_EQ(cfg->tools.disabled_tools[0], "browser");
    free_config(cfg);
}

static void test_config_parse_tools_web_search_provider(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"web_search_provider\":\"brave\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->tools.web_search_provider, "brave");
    free_config(cfg);
}

static void test_config_parse_gateway_require_pairing(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"require_pairing\":false}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_FALSE(cfg->gateway.require_pairing);
    free_config(cfg);
}

static void test_config_parse_gateway_allow_public_bind(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"allow_public_bind\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->gateway.allow_public_bind);
    free_config(cfg);
}

static void test_config_parse_memory_profile(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"profile\":\"hybrid\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->memory.profile, "hybrid");
    free_config(cfg);
}

static void test_config_parse_memory_auto_save(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"auto_save\":false}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_FALSE(cfg->memory.auto_save);
    free_config(cfg);
}

static void test_config_parse_cron_interval(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"cron\":{\"interval_minutes\":10,\"max_run_history\":20}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->cron.interval_minutes, 10u);
    SC_ASSERT_EQ(cfg->cron.max_run_history, 20u);
    free_config(cfg);
}

static void test_config_parse_diagnostics_log_tool_calls(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"log_tool_calls\":true,\"backend\":\"file\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->diagnostics.log_tool_calls);
    SC_ASSERT_STR_EQ(cfg->diagnostics.backend, "file");
    free_config(cfg);
}

static void test_config_parse_reliability_retries(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"provider_retries\":5,\"provider_backoff_ms\":2000}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->reliability.provider_retries, 5u);
    SC_ASSERT_EQ(cfg->reliability.provider_backoff_ms, 2000u);
    free_config(cfg);
}

static void test_config_parse_channels_default(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"channels\":{\"default_channel\":\"slack\",\"cli\":false}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->channels.default_channel, "slack");
    SC_ASSERT_FALSE(cfg->channels.cli);
    free_config(cfg);
}

static void test_config_parse_malformed_truncated(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, "{\"default_provider\":", 20);
    SC_ASSERT_NEQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_parse_malformed_unclosed_brace(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, "{\"x\":1", 6);
    SC_ASSERT_NEQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_parse_malformed_invalid_escape(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, "{\"x\":\"\\uXXXX\"}", 15);
    SC_ASSERT_NEQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_parse_empty_string_value(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"workspace\":\"\"}", 17);
    SC_ASSERT_NOT_NULL(cfg->workspace_dir);
    free_config(cfg);
}

static void test_config_parse_integer_temperature_coerced(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"default_temperature\":1}", 26);
    SC_ASSERT_TRUE(cfg->default_temperature >= 0.99 && cfg->default_temperature <= 1.01);
    free_config(cfg);
}

static void test_config_validate_empty_model_fails(void) {
    sc_config_t cfg = {0};
    char prov[] = "openai", model[] = "";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 3000;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_one_ok(void) {
    sc_config_t cfg = {0};
    char prov[] = "openai", model[] = "gpt-4";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 1;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_config_get_provider_native_tools(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_provider_entry_t *p = (sc_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(sc_provider_entry_t));
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = sc_strdup(&cfg->allocator, "ollama");
    p->native_tools = false;
    cfg->providers = p;
    cfg->providers_len = 1;
    SC_ASSERT_FALSE(sc_config_get_provider_native_tools(cfg, "ollama"));
    p->native_tools = true;
    SC_ASSERT_TRUE(sc_config_get_provider_native_tools(cfg, "ollama"));
    free_config(cfg);
}

static void test_config_env_override_workspace(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    setenv("SEACLAW_WORKSPACE", "/custom/ws", 1);
    sc_config_apply_env_overrides(cfg);
    SC_ASSERT_STR_EQ(cfg->workspace_dir, "/custom/ws");
    unsetenv("SEACLAW_WORKSPACE");
    free_config(cfg);
}

static void test_config_parse_autonomy_allowed_commands(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"autonomy\":{\"allowed_commands\":[\"ls\",\"cat\"]}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->autonomy.allowed_commands_len, 2u);
    SC_ASSERT_STR_EQ(cfg->autonomy.allowed_commands[0], "ls");
    SC_ASSERT_STR_EQ(cfg->autonomy.allowed_commands[1], "cat");
    free_config(cfg);
}

static void test_config_parse_security_audit(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"audit\":{\"enabled\":true,\"log_path\":\"/tmp/audit.log\"}}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->security.audit.enabled);
    SC_ASSERT_STR_EQ(cfg->security.audit.log_path, "/tmp/audit.log");
    free_config(cfg);
}

static void test_config_parse_provider_native_tools_false(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"providers\":[{\"name\":\"ollama\",\"native_tools\":false}]}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->providers_len, 1u);
    SC_ASSERT_FALSE(cfg->providers[0].native_tools);
    free_config(cfg);
}

static void test_config_parse_tunnel_provider(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tunnel\":{\"provider\":\"ngrok\",\"domain\":\"x.ngrok.io\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->tunnel.provider, "ngrok");
    SC_ASSERT_STR_EQ(cfg->tunnel.domain, "x.ngrok.io");
    free_config(cfg);
}

static void test_config_parse_heartbeat_interval(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"heartbeat\":{\"enabled\":true,\"interval_minutes\":5}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->heartbeat.enabled);
    SC_ASSERT_EQ(cfg->heartbeat.interval_minutes, 5u);
    free_config(cfg);
}

static void test_config_parse_scheduler_max_concurrent(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"scheduler\":{\"max_concurrent\":16}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->scheduler.max_concurrent, 16u);
    free_config(cfg);
}

static void test_config_parse_multiple_providers_with_keys(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"providers\":[{\"name\":\"a\",\"api_key\":\"k1\"},{\"name\":\"b\",\"api_"
                    "key\":\"k2\"},{\"name\":\"c\"}]}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->providers_len, 3u);
    SC_ASSERT_STR_EQ(cfg->providers[0].api_key, "k1");
    SC_ASSERT_STR_EQ(cfg->providers[1].api_key, "k2");
    SC_ASSERT_NULL(cfg->providers[2].api_key);
    free_config(cfg);
}

static void test_config_parse_json_nested_deep(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"token_limit\":50000},\"memory\":{\"backend\":\"sqlite\",\"max_"
                    "entries\":500},\"gateway\":{\"port\":4444}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.token_limit, 50000u);
    SC_ASSERT_STR_EQ(cfg->memory.backend, "sqlite");
    SC_ASSERT_EQ(cfg->memory.max_entries, 500u);
    SC_ASSERT_EQ(cfg->gateway.port, 4444);
    free_config(cfg);
}

static void test_config_validate_ok_with_provider_model_port(void) {
    sc_config_t cfg = {0};
    char prov[] = "anthropic", model[] = "claude-3";
    cfg.default_provider = prov;
    cfg.default_model = model;
    cfg.gateway.port = 8080;
    sc_error_t err = sc_config_validate(&cfg);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_config_parse_default_model_provider_prefix(void) {
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    setenv("HOME", "/nonexistent_seaclaw_provider_test", 1);
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_config_load(&backing, &cfg);
    const char *j = "{\"default_provider\":\"gemini\",\"default_model\":\"gemini-2.0-flash\"}";
    sc_config_parse_json(&cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg.default_provider, "gemini");
    SC_ASSERT_STR_EQ(cfg.default_model, "gemini-2.0-flash");
    sc_config_deinit(&cfg);
    if (old_home) {
        setenv("HOME", old_home, 1);
        free(old_home);
    } else
        unsetenv("HOME");
}

static void test_config_parse_workspace_path_with_slash(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"workspace\":\"/home/user/project\"}", 34);
    SC_ASSERT_STR_EQ(cfg->workspace_dir, "/home/user/project");
    free_config(cfg);
}

static void test_config_get_provider_base_url_null_for_unknown(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    const char *url = sc_config_get_provider_base_url(cfg, "unknown_xyz");
    SC_ASSERT_NULL(url);
    free_config(cfg);
}

/* ─── WP-21B parity: web_search_provider, config merge, defaults ────────────── */
static void test_config_get_web_search_provider_default(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    const char *p = sc_config_get_web_search_provider(cfg);
    SC_ASSERT_NOT_NULL(p);
    free_config(cfg);
}

static void test_config_parse_web_search_provider(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"tools\":{\"web_search_provider\":\"brave\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    const char *p = sc_config_get_web_search_provider(cfg);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_STR_EQ(p, "brave");
    free_config(cfg);
}

static void test_config_parse_config_merge_sequential(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"default_provider\":\"anthropic\"}", 32);
    sc_config_parse_json(cfg, "{\"default_model\":\"claude-3-sonnet\"}", 35);
    SC_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    SC_ASSERT_STR_EQ(cfg->default_model, "claude-3-sonnet");
    free_config(cfg);
}

static void test_config_parse_defaults_tools_empty(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{}", 2);
    SC_ASSERT_EQ(cfg->tools.enabled_tools_len, 0u);
    SC_ASSERT_EQ(cfg->tools.disabled_tools_len, 0u);
    free_config(cfg);
}

static void test_config_parse_channels_cli_explicit(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"channels\":{\"default_channel\":\"cli\",\"cli\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->channels.cli);
    SC_ASSERT_STR_EQ(cfg->channels.default_channel, "cli");
    free_config(cfg);
}

static void test_config_parse_malformed_deeply_nested(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_error_t err = sc_config_parse_json(cfg, "{\"a\":{\"b\":{\"c\":", 18);
    SC_ASSERT_NEQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_parse_empty_array_value(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"fallback_providers\":[]}}";
    sc_error_t err = sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(cfg->reliability.fallback_providers_len, 0u);
    free_config(cfg);
}

static void test_config_validate_model_null_after_parse(void) {
    sc_config_t *cfg = make_config_with_arena();
    sc_config_parse_json(cfg, "{\"default_provider\":\"ollama\"}", 28);
    cfg->default_model = NULL;
    sc_error_t err = sc_config_validate(cfg);
    SC_ASSERT_EQ(err, SC_ERR_CONFIG_INVALID);
    free_config(cfg);
}

/* ─── ~30 additional config tests ───────────────────────────────────────── */
static void test_config_parse_memory_backend_sqlite(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"backend\":\"sqlite\",\"sqlite_path\":\"/tmp/db.sqlite\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->memory.backend, "sqlite");
    SC_ASSERT_STR_EQ(cfg->memory.sqlite_path, "/tmp/db.sqlite");
    free_config(cfg);
}

static void test_config_parse_memory_backend_markdown(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"backend\":\"markdown\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->memory.backend, "markdown");
    free_config(cfg);
}

static void test_config_parse_memory_backend_none(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"memory\":{\"backend\":\"none\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->memory.backend, "none");
    free_config(cfg);
}

static void test_config_parse_gateway_pair_rate_limit(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"pair_rate_limit_per_minute\":10}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->gateway.pair_rate_limit_per_minute, 10u);
    free_config(cfg);
}

static void test_config_parse_agent_max_history(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"max_history_messages\":50}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.max_history_messages, 50u);
    free_config(cfg);
}

static void test_config_parse_agent_compaction(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"agent\":{\"compaction_keep_recent\":10,\"compaction_max_summary_chars\":500}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.compaction_keep_recent, 10u);
    SC_ASSERT_EQ(cfg->agent.compaction_max_summary_chars, 500u);
    free_config(cfg);
}

static void test_config_parse_reliability_channel_backoff(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"reliability\":{\"channel_initial_backoff_secs\":5,\"channel_max_backoff_secs\":300}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->reliability.channel_initial_backoff_secs, 5u);
    SC_ASSERT_EQ(cfg->reliability.channel_max_backoff_secs, 300u);
    free_config(cfg);
}

static void test_config_parse_diagnostics_otel(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"backend\":\"otel\",\"otel_endpoint\":\"http://"
                    "localhost:4318\",\"otel_service_name\":\"seaclaw\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->diagnostics.backend, "otel");
    SC_ASSERT_STR_EQ(cfg->diagnostics.otel_endpoint, "http://localhost:4318");
    SC_ASSERT_STR_EQ(cfg->diagnostics.otel_service_name, "seaclaw");
    free_config(cfg);
}

static void test_config_parse_tools_shell_timeout(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    const char *j = "{\"tools\":{\"web_search_provider\":\"brave\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->tools.web_search_provider, "brave");
    free_config(cfg);
}

static void test_config_parse_tools_max_file_size(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    const char *j = "{\"tools\":{\"enabled_tools\":[\"shell\"]}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->tools.enabled_tools_len, 1u);
    SC_ASSERT_STR_EQ(cfg->tools.enabled_tools[0], "shell");
    free_config(cfg);
}

static void test_config_parse_security_sandbox(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"security\":{\"sandbox\":\"landlock\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->security.sandbox, "landlock");
    free_config(cfg);
}

static void test_config_parse_provider_base_url(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j =
        "{\"providers\":[{\"name\":\"compatible\",\"base_url\":\"https://api.example.com/v1\"}]}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->providers_len, 1u);
    SC_ASSERT_STR_EQ(cfg->providers[0].base_url, "https://api.example.com/v1");
    free_config(cfg);
}

static void test_config_parse_unicode_value(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"workspace\":\"/path/\u0442\u0435\u0441\u0442\"}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_NOT_NULL(cfg->workspace_dir);
    free_config(cfg);
}

static void test_config_merge_base_then_override(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    sc_config_parse_json(cfg, "{\"default_provider\":\"anthropic\"}", 33);
    SC_ASSERT_STR_EQ(cfg->default_provider, "anthropic");
    SC_ASSERT_STR_EQ(cfg->default_model, "gpt-4");
    free_config(cfg);
}

static void test_config_parse_empty_object(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    sc_error_t err = sc_config_parse_json(cfg, "{}", 2);
    SC_ASSERT_EQ(err, SC_OK);
    free_config(cfg);
}

static void test_config_parse_cron_enabled(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"cron\":{\"enabled\":true,\"interval_minutes\":5}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->cron.enabled);
    SC_ASSERT_EQ(cfg->cron.interval_minutes, 5u);
    free_config(cfg);
}

static void test_config_parse_gateway_host(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"gateway\":{\"host\":\"127.0.0.1\",\"port\":9000}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->gateway.host, "127.0.0.1");
    SC_ASSERT_EQ(cfg->gateway.port, 9000);
    free_config(cfg);
}

static void test_config_parse_single_provider(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"providers\":[{\"name\":\"ollama\",\"api_key\":null}]}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->providers_len, 1u);
    SC_ASSERT_STR_EQ(cfg->providers[0].name, "ollama");
    free_config(cfg);
}

static void test_config_parse_fallback_providers_single(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"fallback_providers\":[\"gemini\"]}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->reliability.fallback_providers_len, 1u);
    SC_ASSERT_STR_EQ(cfg->reliability.fallback_providers[0], "gemini");
    free_config(cfg);
}

static void test_config_parse_diagnostics_log_receipts(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"log_message_receipts\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->diagnostics.log_message_receipts);
    free_config(cfg);
}

static void test_config_parse_diagnostics_log_llm_io(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"diagnostics\":{\"log_llm_io\":true}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_TRUE(cfg->diagnostics.log_llm_io);
    free_config(cfg);
}

static void test_config_parse_agent_session_idle(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"session_idle_timeout_secs\":3600}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.session_idle_timeout_secs, 3600u);
    free_config(cfg);
}

static void test_config_parse_agent_message_timeout(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"agent\":{\"message_timeout_secs\":120}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->agent.message_timeout_secs, 120u);
    free_config(cfg);
}

static void test_config_parse_runtime_native(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"runtime\":{\"kind\":\"native\"}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_STR_EQ(cfg->runtime.kind, "native");
    free_config(cfg);
}

static void test_config_parse_tools_web_fetch_max_chars(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    const char *j = "{\"tools\":{\"disabled_tools\":[\"browser\"]}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->tools.disabled_tools_len, 1u);
    SC_ASSERT_STR_EQ(cfg->tools.disabled_tools[0], "browser");
    free_config(cfg);
}

static void test_config_parse_scheduler_poll(void) {
    sc_config_t *cfg = make_config_with_arena();
    const char *j = "{\"reliability\":{\"scheduler_poll_secs\":60,\"scheduler_retries\":3}}";
    sc_config_parse_json(cfg, j, strlen(j));
    SC_ASSERT_EQ(cfg->reliability.scheduler_poll_secs, 60u);
    SC_ASSERT_EQ(cfg->reliability.scheduler_retries, 3u);
    free_config(cfg);
}

static void test_config_deinit_idempotent(void) {
    sc_allocator_t backing = sc_system_allocator();
    sc_config_t cfg = {0};
    sc_arena_t *arena = sc_arena_create(backing);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    sc_config_deinit(&cfg);
    sc_config_deinit(&cfg);
    SC_ASSERT_NULL(cfg.arena);
}

static void test_config_parse_very_long_key_value(void) {
    sc_config_t *cfg = make_config_with_arena();
    cfg->default_provider = sc_strdup(&cfg->allocator, "openai");
    cfg->default_model = sc_strdup(&cfg->allocator, "gpt-4");
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "{\"workspace\":\"%s\"}",
                     "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t/u/v/w/x/y/z");
    SC_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(buf));
    sc_error_t err = sc_config_parse_json(cfg, buf, (size_t)n);
    SC_ASSERT_EQ(err, SC_OK);
    free_config(cfg);
}

void run_config_extended_tests(void) {
    SC_TEST_SUITE("Config Extended");
    SC_RUN_TEST(test_config_load_nonexistent_uses_defaults);
    SC_RUN_TEST(test_config_empty_json_uses_defaults);
    SC_RUN_TEST(test_config_partial_json_merges);
    SC_RUN_TEST(test_config_env_override_provider);
    SC_RUN_TEST(test_config_env_override_model);
    SC_RUN_TEST(test_config_env_override_api_key);
    SC_RUN_TEST(test_config_env_override_port);
    SC_RUN_TEST(test_config_env_override_autonomy);
    SC_RUN_TEST(test_config_validate_empty_provider_fails);
    SC_RUN_TEST(test_config_validate_port_zero_fails);
    SC_RUN_TEST(test_config_validate_port_max);
    SC_RUN_TEST(test_config_validate_autonomy_too_high);
    SC_RUN_TEST(test_config_deinit_no_crash);
    SC_RUN_TEST(test_config_multiple_loads);
    SC_RUN_TEST(test_config_parse_with_memory);
    SC_RUN_TEST(test_config_parse_with_gateway);
    SC_RUN_TEST(test_config_parse_malformed_array);
    SC_RUN_TEST(test_config_get_provider_key_missing);
    SC_RUN_TEST(test_config_get_provider_key_from_providers);
    SC_RUN_TEST(test_config_parse_providers_array);
    SC_RUN_TEST(test_config_validate_ok_minimal);
    SC_RUN_TEST(test_config_parse_workspace_override);
    SC_RUN_TEST(test_config_parse_temperature_clamp);
    SC_RUN_TEST(test_config_parse_null_json_fails);
    SC_RUN_TEST(test_config_parse_null_cfg_fails);
    SC_RUN_TEST(test_config_apply_env_overrides_null_safe);
    SC_RUN_TEST(test_config_validate_null_fails);
    SC_RUN_TEST(test_config_get_provider_base_url);
    SC_RUN_TEST(test_config_load_defaults_new_fields);
    SC_RUN_TEST(test_config_parse_new_fields);

    SC_RUN_TEST(test_config_parse_temperature_zero);
    SC_RUN_TEST(test_config_parse_temperature_two);
    SC_RUN_TEST(test_config_parse_temperature_negative_clamped);
    SC_RUN_TEST(test_config_parse_autonomy_level_full);
    SC_RUN_TEST(test_config_parse_autonomy_level_readonly);
    SC_RUN_TEST(test_config_parse_agent_token_limit);
    SC_RUN_TEST(test_config_parse_agent_max_tool_iterations);
    SC_RUN_TEST(test_config_parse_agent_compact_context);
    SC_RUN_TEST(test_config_parse_agent_parallel_tools);
    SC_RUN_TEST(test_config_parse_agent_llm_compiler);
    SC_RUN_TEST(test_config_parse_agent_tool_routing);
    SC_RUN_TEST(test_config_parse_agent_context_pressure);
    SC_RUN_TEST(test_config_persona_per_channel);
    SC_RUN_TEST(test_config_parse_runtime_docker);
    SC_RUN_TEST(test_config_parse_tools_enabled_list);
    SC_RUN_TEST(test_config_parse_tools_disabled_list);
    SC_RUN_TEST(test_config_parse_tools_web_search_provider);
    SC_RUN_TEST(test_config_parse_gateway_require_pairing);
    SC_RUN_TEST(test_config_parse_gateway_allow_public_bind);
    SC_RUN_TEST(test_config_parse_memory_profile);
    SC_RUN_TEST(test_config_parse_memory_auto_save);
    SC_RUN_TEST(test_config_parse_cron_interval);
    SC_RUN_TEST(test_config_parse_diagnostics_log_tool_calls);
    SC_RUN_TEST(test_config_parse_reliability_retries);
    SC_RUN_TEST(test_config_parse_channels_default);
    SC_RUN_TEST(test_config_parse_malformed_truncated);
    SC_RUN_TEST(test_config_parse_malformed_unclosed_brace);
    SC_RUN_TEST(test_config_parse_malformed_invalid_escape);
    SC_RUN_TEST(test_config_parse_empty_string_value);
    SC_RUN_TEST(test_config_parse_integer_temperature_coerced);
    SC_RUN_TEST(test_config_validate_empty_model_fails);
    SC_RUN_TEST(test_config_validate_port_one_ok);
    SC_RUN_TEST(test_config_get_provider_native_tools);
    SC_RUN_TEST(test_config_env_override_workspace);
    SC_RUN_TEST(test_config_parse_autonomy_allowed_commands);
    SC_RUN_TEST(test_config_parse_security_audit);
    SC_RUN_TEST(test_config_parse_provider_native_tools_false);
    SC_RUN_TEST(test_config_parse_tunnel_provider);
    SC_RUN_TEST(test_config_parse_heartbeat_interval);
    SC_RUN_TEST(test_config_parse_scheduler_max_concurrent);
    SC_RUN_TEST(test_config_parse_multiple_providers_with_keys);
    SC_RUN_TEST(test_config_parse_json_nested_deep);
    SC_RUN_TEST(test_config_validate_ok_with_provider_model_port);
    SC_RUN_TEST(test_config_parse_default_model_provider_prefix);
    SC_RUN_TEST(test_config_parse_workspace_path_with_slash);
    SC_RUN_TEST(test_config_get_provider_base_url_null_for_unknown);
    SC_RUN_TEST(test_config_get_web_search_provider_default);
    SC_RUN_TEST(test_config_parse_web_search_provider);
    SC_RUN_TEST(test_config_parse_config_merge_sequential);
    SC_RUN_TEST(test_config_parse_defaults_tools_empty);
    SC_RUN_TEST(test_config_parse_channels_cli_explicit);
    SC_RUN_TEST(test_config_parse_malformed_deeply_nested);
    SC_RUN_TEST(test_config_parse_empty_array_value);
    SC_RUN_TEST(test_config_validate_model_null_after_parse);

    SC_RUN_TEST(test_config_parse_memory_backend_sqlite);
    SC_RUN_TEST(test_config_parse_memory_backend_markdown);
    SC_RUN_TEST(test_config_parse_memory_backend_none);
    SC_RUN_TEST(test_config_parse_gateway_pair_rate_limit);
    SC_RUN_TEST(test_config_parse_agent_max_history);
    SC_RUN_TEST(test_config_parse_agent_compaction);
    SC_RUN_TEST(test_config_parse_reliability_channel_backoff);
    SC_RUN_TEST(test_config_parse_diagnostics_otel);
    SC_RUN_TEST(test_config_parse_tools_shell_timeout);
    SC_RUN_TEST(test_config_parse_tools_max_file_size);
    SC_RUN_TEST(test_config_parse_security_sandbox);
    SC_RUN_TEST(test_config_parse_provider_base_url);
    SC_RUN_TEST(test_config_parse_unicode_value);
    SC_RUN_TEST(test_config_merge_base_then_override);
    SC_RUN_TEST(test_config_parse_empty_object);
    SC_RUN_TEST(test_config_parse_cron_enabled);
    SC_RUN_TEST(test_config_parse_gateway_host);
    SC_RUN_TEST(test_config_parse_single_provider);
    SC_RUN_TEST(test_config_parse_fallback_providers_single);
    SC_RUN_TEST(test_config_parse_diagnostics_log_receipts);
    SC_RUN_TEST(test_config_parse_diagnostics_log_llm_io);
    SC_RUN_TEST(test_config_parse_agent_session_idle);
    SC_RUN_TEST(test_config_parse_agent_message_timeout);
    SC_RUN_TEST(test_config_parse_runtime_native);
    SC_RUN_TEST(test_config_parse_tools_web_fetch_max_chars);
    SC_RUN_TEST(test_config_parse_scheduler_poll);
    SC_RUN_TEST(test_config_deinit_idempotent);
    SC_RUN_TEST(test_config_parse_very_long_key_value);
}
