/* Strict config schema validation — unknown keys, type checks, value validation. */
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/providers/factory.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Top-level keys read by hu_config_parse_json (derive from config.c) */
static const char *const hu_config_top_keys[] = {
    "workspace",     "default_provider",
    "default_model", "default_temperature",
    "max_tokens",    "api_key",
    "providers",     "autonomy",
    "gateway",       "memory",
    "tools",         "cron",
    "scheduler",     "runtime",
    "tunnel",        "channels",
    "agent",         "heartbeat",
    "reliability",   "router",
    "diagnostics",   "session",
    "peripherals",   "hardware",
    "browser",       "cost",
    "mcp_servers",   "nodes",
    "policy",        "plugins",
    "security",      "secrets",
    "identity",
};
static const size_t hu_config_top_keys_len =
    sizeof(hu_config_top_keys) / sizeof(hu_config_top_keys[0]);

/* Nested keys per section */
static const char *const hu_gateway_keys[] = {
    "enabled",
    "port",
    "host",
    "require_pairing",
    "auth_token",
    "allow_public_bind",
    "pair_rate_limit_per_minute",
    "rate_limit_requests",
    "rate_limit_window",
    "webhook_hmac_secret",
    "control_ui_dir",
    "cors_origins",
};
static const size_t hu_gateway_keys_len = sizeof(hu_gateway_keys) / sizeof(hu_gateway_keys[0]);

static const char *const hu_memory_keys[] = {
    "profile",      "backend",         "sqlite_path",
    "max_entries",  "auto_save",       "consolidation_interval_hours",
    "postgres_url", "postgres_schema", "postgres_table",
    "redis_host",   "redis_port",      "redis_key_prefix",
    "api_base_url", "api_key",         "api_timeout_ms",
};
static const size_t hu_memory_keys_len = sizeof(hu_memory_keys) / sizeof(hu_memory_keys[0]);

static const char *const hu_security_keys[] = {
    "autonomy_level", "sandbox", "sandbox_config", "resources", "audit",
};
static const size_t hu_security_keys_len = sizeof(hu_security_keys) / sizeof(hu_security_keys[0]);

/* Core provider names */
static const char *const hu_known_providers[] = {
    "openai",       "anthropic",  "gemini",     "google",     "google-gemini",
    "ollama",       "openrouter", "compatible", "claude_cli", "codex_cli",
    "openai-codex", "router",     "reliable",
};
static const size_t hu_known_providers_len =
    sizeof(hu_known_providers) / sizeof(hu_known_providers[0]);

static bool key_in_list(const char *key, const char *const *list, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (strcmp(key, list[i]) == 0)
            return true;
    }
    return false;
}

static bool is_provider_valid(const char *name) {
    if (!name || !name[0])
        return false;
    if (key_in_list(name, hu_known_providers, hu_known_providers_len))
        return true;
    if (strncmp(name, "custom:", 7) == 0 || strncmp(name, "anthropic-custom:", 17) == 0)
        return true;
    return hu_compatible_provider_url(name) != NULL;
}

static void check_unknown_top_keys(const hu_json_value_t *root, bool strict, bool *has_error) {
    if (!root || root->type != HU_JSON_OBJECT || !root->data.object.pairs)
        return;
    for (size_t i = 0; i < root->data.object.len; i++) {
        hu_json_pair_t *p = &root->data.object.pairs[i];
        if (!p->key)
            continue;
        if (!key_in_list(p->key, hu_config_top_keys, hu_config_top_keys_len)) {
            fprintf(stderr, "[config] unknown key: '%s' (ignored)\n", p->key);
            if (strict)
                *has_error = true;
        }
    }
}

static void check_unknown_nested_keys(const hu_json_value_t *obj, const char *section,
                                      const char *const *allowed, size_t allowed_len, bool strict,
                                      bool *has_error) {
    if (!obj || obj->type != HU_JSON_OBJECT || !obj->data.object.pairs)
        return;
    for (size_t i = 0; i < obj->data.object.len; i++) {
        hu_json_pair_t *p = &obj->data.object.pairs[i];
        if (!p->key)
            continue;
        if (!key_in_list(p->key, allowed, allowed_len)) {
            fprintf(stderr, "[config] unknown key '%s.%s' (ignored)\n", section, p->key);
            if (strict)
                *has_error = true;
        }
    }
}

static hu_error_t check_type(const hu_json_value_t *obj, const char *key, hu_json_type_t expected,
                             const char *ctx, bool strict) {
    hu_json_value_t *v = hu_json_object_get(obj, key);
    if (!v)
        return HU_OK;
    if (v->type != expected) {
        const char *want = (expected == HU_JSON_STRING)   ? "string"
                           : (expected == HU_JSON_NUMBER) ? "number"
                           : (expected == HU_JSON_BOOL)   ? "boolean"
                                                          : "unknown";
        fprintf(stderr, "[config] %s: '%s' must be %s\n", ctx, key, want);
        return strict ? HU_ERR_CONFIG_INVALID : HU_OK;
    }
    return HU_OK;
}

static bool starts_with(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool has_path_traversal(const char *path) {
    return path && strstr(path, "..") != NULL;
}

hu_error_t hu_config_validate_strict(const hu_config_t *cfg, const hu_json_value_t *root,
                                     bool strict) {
    if (!cfg)
        return HU_ERR_INVALID_ARGUMENT;
    bool has_error = false;

    /* Unknown key detection */
    if (root)
        check_unknown_top_keys(root, strict, &has_error);
    if (root) {
        hu_json_value_t *gw = hu_json_object_get(root, "gateway");
        if (gw)
            check_unknown_nested_keys(gw, "gateway", hu_gateway_keys, hu_gateway_keys_len, strict,
                                      &has_error);
        hu_json_value_t *mem = hu_json_object_get(root, "memory");
        if (mem)
            check_unknown_nested_keys(mem, "memory", hu_memory_keys, hu_memory_keys_len, strict,
                                      &has_error);
        hu_json_value_t *sec = hu_json_object_get(root, "security");
        if (sec)
            check_unknown_nested_keys(sec, "security", hu_security_keys, hu_security_keys_len,
                                      strict, &has_error);
    }

    /* Type checking */
    if (root) {
        hu_error_t err;
        err = check_type(root, "default_provider", HU_JSON_STRING, "default_provider", strict);
        if (err != HU_OK)
            return err;
        err = check_type(root, "default_model", HU_JSON_STRING, "default_model", strict);
        if (err != HU_OK)
            return err;
        err = check_type(root, "max_tokens", HU_JSON_NUMBER, "max_tokens", strict);
        if (err != HU_OK)
            return err;
        hu_json_value_t *gw = hu_json_object_get(root, "gateway");
        if (gw) {
            err = check_type(gw, "port", HU_JSON_NUMBER, "gateway.port", strict);
            if (err != HU_OK)
                return err;
        }
    }

    /* Value validation */
    if (cfg->default_model && !cfg->default_model[0]) {
        fprintf(stderr, "[config] default_model cannot be empty\n");
        if (strict)
            has_error = true;
    }
    if (cfg->default_provider && !is_provider_valid(cfg->default_provider)) {
        fprintf(stderr, "[config] unknown provider: '%s'\n",
                cfg->default_provider ? cfg->default_provider : "(empty)");
        if (strict)
            has_error = true;
    }
    if (cfg->max_tokens != 0 && (cfg->max_tokens < 1 || cfg->max_tokens > 1000000)) {
        fprintf(stderr, "[config] max_tokens (%u) outside 1–1000000 (warning)\n", cfg->max_tokens);
        if (strict)
            has_error = true;
    }
    if (cfg->agent.max_tool_iterations > 10000) {
        fprintf(stderr, "[config] agent.max_tool_iterations (%u) outside 1–10000 (warning)\n",
                cfg->agent.max_tool_iterations);
        if (strict)
            has_error = true;
    }

    /* URL validation */
    if (cfg->memory.api_base_url && strlen(cfg->memory.api_base_url) >= 8) {
        if (!starts_with(cfg->memory.api_base_url, "https://")) {
            fprintf(stderr, "[config] memory.api_base_url must use https://\n");
            if (strict)
                has_error = true;
        }
    }
    if (cfg->diagnostics.otel_endpoint && strlen(cfg->diagnostics.otel_endpoint) >= 8) {
        if (!starts_with(cfg->diagnostics.otel_endpoint, "https://")) {
            fprintf(stderr, "[config] diagnostics.otel_endpoint must use https://\n");
            if (strict)
                has_error = true;
        }
    }
    for (size_t i = 0; cfg->providers && i < cfg->providers_len; i++) {
        const char *url = cfg->providers[i].base_url;
        if (url && strlen(url) >= 8 && !starts_with(url, "https://") &&
            !starts_with(url, "http://localhost") && !starts_with(url, "http://127.0.0.1")) {
            fprintf(stderr, "[config] providers[%zu].base_url must use https:// (or localhost)\n",
                    i);
            if (strict)
                has_error = true;
        }
    }

    /* Path traversal */
    if (cfg->workspace_dir && has_path_traversal(cfg->workspace_dir)) {
        fprintf(stderr, "[config] workspace_dir must not contain '..'\n");
        if (strict)
            has_error = true;
    }
    if (cfg->memory.sqlite_path && has_path_traversal(cfg->memory.sqlite_path)) {
        fprintf(stderr, "[config] memory.sqlite_path must not contain '..'\n");
        if (strict)
            has_error = true;
    }
    if (cfg->security.audit.log_path && has_path_traversal(cfg->security.audit.log_path)) {
        fprintf(stderr, "[config] security.audit.log_path must not contain '..'\n");
        if (strict)
            has_error = true;
    }
    if (cfg->gateway.control_ui_dir && has_path_traversal(cfg->gateway.control_ui_dir)) {
        fprintf(stderr, "[config] gateway.control_ui_dir must not contain '..'\n");
        if (strict)
            has_error = true;
    }

    /* Run base validation (provider, model, port) */
    if (has_error)
        return HU_ERR_CONFIG_INVALID;
    if (!cfg->default_provider || !cfg->default_provider[0])
        return HU_ERR_CONFIG_INVALID;
    if (!cfg->default_model || !cfg->default_model[0])
        return HU_ERR_CONFIG_INVALID;
    if (cfg->gateway.port == 0)
        return HU_ERR_CONFIG_INVALID;
    if (cfg->security.autonomy_level > 4)
        return HU_ERR_CONFIG_INVALID;
    return HU_OK;
}
