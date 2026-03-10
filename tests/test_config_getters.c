/* Config getters tests — hu_config_getters.c coverage. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "test_framework.h"
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

/* ─── hu_config_provider_requires_api_key ─────────────────────────────────── */

static void test_provider_requires_api_key_null_returns_true(void) {
    HU_ASSERT_TRUE(hu_config_provider_requires_api_key(NULL));
}

static void test_provider_requires_api_key_local_providers_return_false(void) {
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("ollama"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("lmstudio"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("lm-studio"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("claude_cli"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("codex_cli"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("llamacpp"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("llama.cpp"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("vllm"));
    HU_ASSERT_FALSE(hu_config_provider_requires_api_key("sglang"));
}

static void test_provider_requires_api_key_cloud_providers_return_true(void) {
    HU_ASSERT_TRUE(hu_config_provider_requires_api_key("openai"));
    HU_ASSERT_TRUE(hu_config_provider_requires_api_key("anthropic"));
    HU_ASSERT_TRUE(hu_config_provider_requires_api_key("unknown"));
}

/* ─── hu_config_validate ───────────────────────────────────────────────────── */

static void test_config_validate_null_returns_invalid_argument(void) {
    hu_error_t err = hu_config_validate(NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_config_validate_empty_provider_returns_config_invalid(void) {
    hu_config_t cfg = {0};
    cfg.default_model = "gpt-4";
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_empty_model_returns_config_invalid(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "openai";
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_autonomy_gt_4_returns_config_invalid(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "ollama";
    cfg.default_model = "llama2";
    cfg.gateway.port = 3000;
    cfg.security.autonomy_level = 5;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_port_zero_returns_config_invalid(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "ollama";
    cfg.default_model = "llama2";
    cfg.gateway.port = 0;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_ERR_CONFIG_INVALID);
}

static void test_config_validate_valid_config_returns_ok(void) {
    hu_config_t cfg = {0};
    cfg.default_provider = "ollama";
    cfg.default_model = "llama2";
    cfg.gateway.port = 3000;
    hu_error_t err = hu_config_validate(&cfg);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ─── hu_config_get_provider_key ───────────────────────────────────────────── */

static void test_get_provider_key_null_cfg_returns_null(void) {
    HU_ASSERT_NULL(hu_config_get_provider_key(NULL, "openai"));
}

static void test_get_provider_key_null_name_returns_null(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_NULL(hu_config_get_provider_key(&cfg, NULL));
}

static void test_get_provider_key_from_provider_entry(void) {
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

static void test_get_provider_key_falls_back_to_api_key(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->api_key = hu_strdup(&cfg->allocator, "global-key");
    hu_provider_entry_t *p = (hu_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_provider_entry_t));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strdup(&cfg->allocator, "openai");
    p->api_key = NULL;
    cfg->providers = p;
    cfg->providers_len = 1;
    const char *key = hu_config_get_provider_key(cfg, "openai");
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "global-key");
    free_config(cfg);
}

static void test_get_provider_key_unknown_provider_uses_api_key(void) {
    hu_config_t cfg = {0};
    char api_key[] = "sk-global";
    cfg.api_key = api_key;
    const char *key = hu_config_get_provider_key(&cfg, "nonexistent");
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "sk-global");
}

static void test_get_provider_key_unknown_no_api_key_returns_null(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_NULL(hu_config_get_provider_key(&cfg, "nonexistent"));
}

/* ─── hu_config_default_provider_key ───────────────────────────────────────── */

static void test_default_provider_key_null_cfg_returns_null(void) {
    HU_ASSERT_NULL(hu_config_default_provider_key(NULL));
}

static void test_default_provider_key_returns_key_for_default_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->api_key = hu_strdup(&cfg->allocator, "sk-openai");
    const char *key = hu_config_default_provider_key(cfg);
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "sk-openai");
    free_config(cfg);
}

/* ─── hu_config_get_provider_base_url ──────────────────────────────────────── */

static void test_get_provider_base_url_null_inputs_return_null(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_NULL(hu_config_get_provider_base_url(NULL, "openai"));
    HU_ASSERT_NULL(hu_config_get_provider_base_url(&cfg, NULL));
}

static void test_get_provider_base_url_returns_url_for_provider(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_provider_entry_t *p = (hu_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_provider_entry_t));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strdup(&cfg->allocator, "compatible");
    p->base_url = hu_strdup(&cfg->allocator, "https://api.example.com");
    cfg->providers = p;
    cfg->providers_len = 1;
    const char *url = hu_config_get_provider_base_url(cfg, "compatible");
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_STR_EQ(url, "https://api.example.com");
    free_config(cfg);
}

static void test_get_provider_base_url_unknown_returns_null(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_NULL(hu_config_get_provider_base_url(&cfg, "unknown"));
}

/* ─── hu_config_get_provider_native_tools ──────────────────────────────────── */

static void test_get_provider_native_tools_null_inputs_default_true(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_TRUE(hu_config_get_provider_native_tools(NULL, "openai"));
    HU_ASSERT_TRUE(hu_config_get_provider_native_tools(&cfg, NULL));
}

static void test_get_provider_native_tools_respects_provider_setting(void) {
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

static void test_get_provider_native_tools_unknown_defaults_true(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_TRUE(hu_config_get_provider_native_tools(&cfg, "unknown"));
}

/* ─── hu_config_get_web_search_provider ────────────────────────────────────── */

static void test_get_web_search_provider_null_cfg_returns_duckduckgo(void) {
    unsetenv("WEB_SEARCH_PROVIDER");
    unsetenv("HUMAN_WEB_SEARCH_PROVIDER");
    const char *p = hu_config_get_web_search_provider(NULL);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "duckduckgo");
}

static void test_get_web_search_provider_uses_config_value(void) {
    hu_config_t cfg = {0};
    char provider[] = "brave";
    cfg.tools.web_search_provider = provider;
    unsetenv("WEB_SEARCH_PROVIDER");
    unsetenv("HUMAN_WEB_SEARCH_PROVIDER");
    const char *p = hu_config_get_web_search_provider(&cfg);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "brave");
}

/* ─── hu_config_get_channel_configured_count ──────────────────────────────── */

static void test_get_channel_configured_count_null_inputs_return_zero(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_EQ(hu_config_get_channel_configured_count(NULL, "telegram"), 0u);
    HU_ASSERT_EQ(hu_config_get_channel_configured_count(&cfg, NULL), 0u);
}

static void test_get_channel_configured_count_returns_count_for_key(void) {
    hu_config_t cfg = {0};
    char key_telegram[] = "telegram";
    cfg.channels.channel_config_keys[0] = key_telegram;
    cfg.channels.channel_config_counts[0] = 3;
    cfg.channels.channel_config_len = 1;
    HU_ASSERT_EQ(hu_config_get_channel_configured_count(&cfg, "telegram"), 3u);
}

static void test_get_channel_configured_count_unknown_returns_zero(void) {
    hu_config_t cfg = {0};
    cfg.channels.channel_config_len = 0;
    HU_ASSERT_EQ(hu_config_get_channel_configured_count(&cfg, "unknown"), 0u);
}

/* ─── hu_config_get_provider_ws_streaming ──────────────────────────────────── */

static void test_get_provider_ws_streaming_null_inputs_return_false(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_FALSE(hu_config_get_provider_ws_streaming(NULL, "openai"));
    HU_ASSERT_FALSE(hu_config_get_provider_ws_streaming(&cfg, NULL));
}

static void test_get_provider_ws_streaming_respects_provider_setting(void) {
    hu_config_t *cfg = make_config_with_arena();
    hu_provider_entry_t *p = (hu_provider_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_provider_entry_t));
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0, sizeof(*p));
    p->name = hu_strdup(&cfg->allocator, "test");
    p->ws_streaming = true;
    cfg->providers = p;
    cfg->providers_len = 1;
    HU_ASSERT_TRUE(hu_config_get_provider_ws_streaming(cfg, "test"));
    free_config(cfg);
}

static void test_get_provider_ws_streaming_unknown_defaults_false(void) {
    hu_config_t cfg = {0};
    HU_ASSERT_FALSE(hu_config_get_provider_ws_streaming(&cfg, "unknown"));
}

/* ─── hu_config_persona_for_channel ────────────────────────────────────────── */

static void test_persona_for_channel_null_cfg_returns_null(void) {
    HU_ASSERT_NULL(hu_config_persona_for_channel(NULL, "telegram"));
}

static void test_persona_for_channel_channel_override(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->agent.persona = hu_strdup(&cfg->allocator, "default");
    hu_persona_channel_entry_t *entries = (hu_persona_channel_entry_t *)cfg->allocator.alloc(
        cfg->allocator.ctx, sizeof(hu_persona_channel_entry_t) * 2);
    HU_ASSERT_NOT_NULL(entries);
    memset(entries, 0, sizeof(hu_persona_channel_entry_t) * 2);
    entries[0].channel = hu_strdup(&cfg->allocator, "imessage");
    entries[0].persona = hu_strdup(&cfg->allocator, "casual");
    entries[1].channel = hu_strdup(&cfg->allocator, "gmail");
    entries[1].persona = hu_strdup(&cfg->allocator, "professional");
    cfg->agent.persona_channels = entries;
    cfg->agent.persona_channels_count = 2;

    const char *im = hu_config_persona_for_channel(cfg, "imessage");
    HU_ASSERT_NOT_NULL(im);
    HU_ASSERT_STR_EQ(im, "casual");

    const char *gm = hu_config_persona_for_channel(cfg, "gmail");
    HU_ASSERT_NOT_NULL(gm);
    HU_ASSERT_STR_EQ(gm, "professional");

    free_config(cfg);
}

static void test_persona_for_channel_falls_back_to_default(void) {
    hu_config_t cfg = {0};
    char default_persona[] = "seth";
    cfg.agent.persona = default_persona;
    cfg.agent.persona_channels = NULL;
    cfg.agent.persona_channels_count = 0;

    const char *p = hu_config_persona_for_channel(&cfg, "telegram");
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "seth");

    const char *p_null = hu_config_persona_for_channel(&cfg, NULL);
    HU_ASSERT_NOT_NULL(p_null);
    HU_ASSERT_STR_EQ(p_null, "seth");
}

/* ─── hu_config_set_reload_requested / hu_config_get_and_clear_reload_requested ─ */

static void test_reload_flag_set_then_get_returns_true(void) {
    hu_config_get_and_clear_reload_requested();
    hu_config_set_reload_requested();
    HU_ASSERT_TRUE(hu_config_get_and_clear_reload_requested());
}

static void test_reload_flag_get_again_returns_false_cleared(void) {
    hu_config_get_and_clear_reload_requested();
    hu_config_set_reload_requested();
    (void)hu_config_get_and_clear_reload_requested();
    HU_ASSERT_FALSE(hu_config_get_and_clear_reload_requested());
}

/* ─── hu_config_get_tool_model_override ─────────────────────────────────────── */

static void test_config_get_tool_model_override_null_cfg(void) {
    const char *prov = NULL, *mod = NULL;
    HU_ASSERT_FALSE(hu_config_get_tool_model_override(NULL, "web_search", &prov, &mod));
}

static void test_config_get_tool_model_override_null_tool(void) {
    hu_config_t cfg = {0};
    const char *prov = NULL, *mod = NULL;
    HU_ASSERT_FALSE(hu_config_get_tool_model_override(&cfg, NULL, &prov, &mod));
}

static void test_config_get_tool_model_override_not_found(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *prov = NULL, *mod = NULL;
    HU_ASSERT_FALSE(hu_config_get_tool_model_override(cfg, "web_search", &prov, &mod));
    free_config(cfg);
}

static void test_config_get_tool_model_override_found(void) {
    hu_config_t *cfg = make_config_with_arena();
    cfg->default_provider = hu_strdup(&cfg->allocator, "openai");
    cfg->default_model = hu_strdup(&cfg->allocator, "gpt-4");
    const char *json = "{\"tools\":{\"tool_model_overrides\":{"
                       "\"web_search\":{\"provider\":\"gemini\",\"model\":\"gemini-2.5-flash\"},"
                       "\"shell\":{\"provider\":\"anthropic\",\"model\":\"claude-sonnet-4.6\"}"
                       "}}}";
    hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    const char *prov = NULL, *mod = NULL;
    HU_ASSERT_TRUE(hu_config_get_tool_model_override(cfg, "web_search", &prov, &mod));
    HU_ASSERT_NOT_NULL(prov);
    HU_ASSERT_NOT_NULL(mod);
    HU_ASSERT_STR_EQ(prov, "gemini");
    HU_ASSERT_STR_EQ(mod, "gemini-2.5-flash");
    prov = NULL;
    mod = NULL;
    HU_ASSERT_TRUE(hu_config_get_tool_model_override(cfg, "shell", &prov, &mod));
    HU_ASSERT_NOT_NULL(prov);
    HU_ASSERT_NOT_NULL(mod);
    HU_ASSERT_STR_EQ(prov, "anthropic");
    HU_ASSERT_STR_EQ(mod, "claude-sonnet-4.6");
    free_config(cfg);
}

/* ─── run ──────────────────────────────────────────────────────────────────── */

void run_config_getters_tests(void) {
    HU_TEST_SUITE("Config getters");
    HU_RUN_TEST(test_provider_requires_api_key_null_returns_true);
    HU_RUN_TEST(test_provider_requires_api_key_local_providers_return_false);
    HU_RUN_TEST(test_provider_requires_api_key_cloud_providers_return_true);
    HU_RUN_TEST(test_config_validate_null_returns_invalid_argument);
    HU_RUN_TEST(test_config_validate_empty_provider_returns_config_invalid);
    HU_RUN_TEST(test_config_validate_empty_model_returns_config_invalid);
    HU_RUN_TEST(test_config_validate_autonomy_gt_4_returns_config_invalid);
    HU_RUN_TEST(test_config_validate_port_zero_returns_config_invalid);
    HU_RUN_TEST(test_config_validate_valid_config_returns_ok);
    HU_RUN_TEST(test_get_provider_key_null_cfg_returns_null);
    HU_RUN_TEST(test_get_provider_key_null_name_returns_null);
    HU_RUN_TEST(test_get_provider_key_from_provider_entry);
    HU_RUN_TEST(test_get_provider_key_falls_back_to_api_key);
    HU_RUN_TEST(test_get_provider_key_unknown_provider_uses_api_key);
    HU_RUN_TEST(test_get_provider_key_unknown_no_api_key_returns_null);
    HU_RUN_TEST(test_default_provider_key_null_cfg_returns_null);
    HU_RUN_TEST(test_default_provider_key_returns_key_for_default_provider);
    HU_RUN_TEST(test_get_provider_base_url_null_inputs_return_null);
    HU_RUN_TEST(test_get_provider_base_url_returns_url_for_provider);
    HU_RUN_TEST(test_get_provider_base_url_unknown_returns_null);
    HU_RUN_TEST(test_get_provider_native_tools_null_inputs_default_true);
    HU_RUN_TEST(test_get_provider_native_tools_respects_provider_setting);
    HU_RUN_TEST(test_get_provider_native_tools_unknown_defaults_true);
    HU_RUN_TEST(test_get_web_search_provider_null_cfg_returns_duckduckgo);
    HU_RUN_TEST(test_get_web_search_provider_uses_config_value);
    HU_RUN_TEST(test_get_channel_configured_count_null_inputs_return_zero);
    HU_RUN_TEST(test_get_channel_configured_count_returns_count_for_key);
    HU_RUN_TEST(test_get_channel_configured_count_unknown_returns_zero);
    HU_RUN_TEST(test_get_provider_ws_streaming_null_inputs_return_false);
    HU_RUN_TEST(test_get_provider_ws_streaming_respects_provider_setting);
    HU_RUN_TEST(test_get_provider_ws_streaming_unknown_defaults_false);
    HU_RUN_TEST(test_persona_for_channel_null_cfg_returns_null);
    HU_RUN_TEST(test_persona_for_channel_channel_override);
    HU_RUN_TEST(test_persona_for_channel_falls_back_to_default);
    HU_RUN_TEST(test_reload_flag_set_then_get_returns_true);
    HU_RUN_TEST(test_reload_flag_get_again_returns_false_cleared);
    HU_RUN_TEST(test_config_get_tool_model_override_null_cfg);
    HU_RUN_TEST(test_config_get_tool_model_override_null_tool);
    HU_RUN_TEST(test_config_get_tool_model_override_not_found);
    HU_RUN_TEST(test_config_get_tool_model_override_found);
}
