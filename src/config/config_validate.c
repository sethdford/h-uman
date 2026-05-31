/* Strict config schema validation — unknown keys, type checks, value validation. */
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/providers/factory.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Top-level keys read by hu_config_parse_json (derive from config.c) */
static const char *const hu_config_top_keys[] = {
    "workspace",
    "dpo_export_dir",
    "default_provider",
    "default_model",
    "default_temperature",
    "max_tokens",
    "api_key",
    "providers",
    "autonomy",
    "gateway",
    "memory",
    "tools",
    "cron",
    "scheduler",
    "inference",
    "runtime",
    "tunnel",
    "channels",
    "agent",
    "heartbeat",
    "reliability",
    "router",
    "ensemble",
    "diagnostics",
    "session",
    "peripherals",
    "hardware",
    "browser",
    "cost",
    "mcp_servers",
    "nodes",
    "policy",
    "plugins",
    "security",
    "secrets",
    "identity",
    "feeds",
    "voice",
    "provider",
    "model",
    "auto_update",
    "update_check_interval_hours",
    "config_version",
    "data_dir",
    "temp_dir",
    "behavior",
    "mlx_local",
    /* 2026-05-18: reaction_collection parser exists in src/config_parse.c
     * but was missing from this whitelist, causing the validator to log
     * "unknown key: 'reaction_collection' (ignored)" on every daemon startup
     * even though the key WAS being parsed correctly. The misleading log was
     * the smoking gun for the
     * `.claude/rules/silent-config-gated-subsystems.md` audit that surfaced
     * the bigger issue (whole subsystem disabled by missing config block). */
    "reaction_collection",
    "personalization",
    /* 2026-05-19: Spec 2 (reaction-loop pair-count trigger) added the
     * `learning` block with `dpo_pair_training_threshold` and
     * `m3_frontier_auto_training`. Spec 1 added `inference` for the
     * best-of-N at inference (US-7.7). Without these in the whitelist,
     * the validator emits "unknown key" even though config_parse.c
     * picks them up correctly. */
    "learning",
    "intrinsic", /* A3 intrinsic motivation — docs/plans/2026-05-29-intrinsic-motivation/ */
    "prosocial_routines", /* C-series — docs/plans/2026-05-29-prosocial-uplift/ */
    "inference",
    /* 2026-05-25: initiative layer (proactive proposer skeleton). See
     * docs/plans/2026-05-25-initiative-layer/. Disabled by default — flipping
     * cfg->initiative.enabled=true in config.json starts the periodic tick. */
    "initiative",
    /* 2026-05-25: prompt-budget compression — observability for the
     * hu_prompt_build_system per-field byte distribution. See
     * docs/plans/2026-05-25-director-compression/. Disabled by default;
     * Phase 1 ships the measurement, Phase 2 wires the trim. */
    "prompt_budget",
    /* 2026-05-26: M3 Bridge B Phase B4 — mlx_local SSE streaming gate
     * + first-token budget. See docs/plans/2026-05-26-m3-b4-mlx-local-sse/.
     * streaming_enabled defaults true; operator opts out via
     * {"mlx_local": {"streaming_enabled": false}} to diagnose
     * streaming-specific regressions. */
    "mlx_local",
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

static const char *const hu_ensemble_keys[] = {
    "providers",
    "strategy",
    "routing",
};
static const size_t hu_ensemble_keys_len = sizeof(hu_ensemble_keys) / sizeof(hu_ensemble_keys[0]);

static const char *const hu_voice_keys[] = {
    "local_stt_endpoint", "local_tts_endpoint", "stt_provider", "tts_provider", "tts_voice",
    "tts_model",          "stt_model",          "stt_language", "mode",         "realtime_model",
    "realtime_voice",
};
static const size_t hu_voice_keys_len = sizeof(hu_voice_keys) / sizeof(hu_voice_keys[0]);

static const char *const hu_security_keys[] = {
    "autonomy_level", "sandbox", "sandbox_config", "resources", "audit",
};
static const size_t hu_security_keys_len = sizeof(hu_security_keys) / sizeof(hu_security_keys[0]);

/* Core provider names */
static const char *const hu_known_providers[] = {
    "openai",     "anthropic",  "gemini",     "google",    "google-gemini", "ollama",
    "openrouter", "compatible", "claude_cli", "codex_cli", "openai-codex",  "router",
    "reliable",   "ensemble",   "mlx_local",  "mlx-local", "apple",
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

/* Cap on collected unknown keys for the consolidated startup banner. Bounds
 * memory in pathological/adversarial configs; extra keys still get the
 * per-key info log, just not the banner mention. */
#define HU_CONFIG_UNKNOWN_KEY_BANNER_CAP 16

static void check_unknown_top_keys(const hu_json_value_t *root, bool strict, bool *has_error) {
    if (!root || root->type != HU_JSON_OBJECT || !root->data.object.pairs)
        return;

    /* Collect unknown keys so we can also emit one consolidated banner line.
     * Per ~/.claude/rules/silent-config-gated-subsystems.md, operators easily
     * miss per-key info lines in multi-thousand-line service logs; the banner
     * surfaces "your binary doesn't understand these keys" as one warn line. */
    const char *unknown_keys[HU_CONFIG_UNKNOWN_KEY_BANNER_CAP];
    size_t unknown_count = 0;
    size_t unknown_total = 0;

    for (size_t i = 0; i < root->data.object.len; i++) {
        hu_json_pair_t *p = &root->data.object.pairs[i];
        if (!p->key)
            continue;
        if (!key_in_list(p->key, hu_config_top_keys, hu_config_top_keys_len)) {
            /* Per-key info-level line (existing contract — keep so operators
             * can still grep each key individually). */
            hu_log_info("config", NULL, "unknown key: '%s' (ignored)", p->key);
            unknown_total++;
            if (unknown_count < HU_CONFIG_UNKNOWN_KEY_BANNER_CAP)
                unknown_keys[unknown_count++] = p->key;
            if (strict)
                *has_error = true;
        }
    }

    if (unknown_total == 0)
        return;

    /* Build the consolidated banner. Cap each key length defensively so an
     * adversarial JSON with a giant key can't blow the message buffer. */
    char buf[512];
    size_t off = 0;
    int n = snprintf(buf + off, sizeof(buf) - off,
                     "WARNING: %zu unknown config %s ignored: ", unknown_total,
                     unknown_total == 1 ? "key" : "keys");
    if (n < 0 || (size_t)n >= sizeof(buf) - off)
        return;
    off += (size_t)n;
    for (size_t i = 0; i < unknown_count && off < sizeof(buf) - 1; i++) {
        const char *sep = (i + 1 < unknown_count) ? ", " : "";
        n = snprintf(buf + off, sizeof(buf) - off, "%s%s", unknown_keys[i], sep);
        if (n < 0 || (size_t)n >= sizeof(buf) - off)
            break;
        off += (size_t)n;
    }
    if (unknown_total > unknown_count && off < sizeof(buf) - 1) {
        n = snprintf(buf + off, sizeof(buf) - off, " (+%zu more)", unknown_total - unknown_count);
        if (n > 0 && (size_t)n < sizeof(buf) - off)
            off += (size_t)n;
    }
    if (off < sizeof(buf) - 1) {
        (void)snprintf(buf + off, sizeof(buf) - off,
                       " - these are either deprecated or your binary "
                       "predates the schema. Rebuild from source or remove them.");
    }
    hu_log_warn("config", NULL, "%s", buf);
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
            hu_log_error("config", NULL, "unknown key '%s.%s' (ignored)", section, p->key);
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
        hu_log_info("config", NULL, "%s: '%s' must be %s", ctx, key, want);
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

/* Must stay aligned with src/memory/factory.c backend availability. */
static bool memory_backend_ok_for_build(const char *backend) {
    if (!backend || !backend[0])
        return true;
    if (strcmp(backend, "lru") == 0 || strcmp(backend, "memory") == 0 ||
        strcmp(backend, "api") == 0 || strcmp(backend, "markdown") == 0)
        return true;
#if HU_ENABLE_SQLITE
    if (strcmp(backend, "sqlite") == 0)
        return true;
#endif
#if HU_HAS_NONE_ENGINE
    if (strcmp(backend, "none") == 0)
        return true;
#endif
#if HU_HAS_LANCEDB_ENGINE
    if (strcmp(backend, "lancedb") == 0)
        return true;
#endif
#if HU_HAS_LUCID_ENGINE
    if (strcmp(backend, "lucid") == 0)
        return true;
#endif
#if HU_ENABLE_POSTGRES
    if (strcmp(backend, "postgres") == 0)
        return true;
#endif
#if HU_ENABLE_REDIS_ENGINE
    if (strcmp(backend, "redis") == 0)
        return true;
#endif
    return false;
}

static void check_memory_backend_build(const hu_config_t *cfg, bool strict, bool *has_error) {
    const char *b = cfg->memory.backend;
    if (!b || !b[0])
        return;
    if (memory_backend_ok_for_build(b))
        return;
    hu_log_info("config", NULL, "memory.backend '%s' is unknown or not available in this build", b);
    if (strict)
        *has_error = true;
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
        hu_json_value_t *ens = hu_json_object_get(root, "ensemble");
        if (ens)
            check_unknown_nested_keys(ens, "ensemble", hu_ensemble_keys, hu_ensemble_keys_len,
                                      strict, &has_error);
        hu_json_value_t *voice = hu_json_object_get(root, "voice");
        if (voice)
            check_unknown_nested_keys(voice, "voice", hu_voice_keys, hu_voice_keys_len, strict,
                                      &has_error);
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
        hu_log_info("config", NULL, "default_model cannot be empty");
        if (strict)
            has_error = true;
    }
    if (cfg->default_provider && !is_provider_valid(cfg->default_provider)) {
        hu_log_info("config", NULL, "unknown provider: '%s'",
                    cfg->default_provider ? cfg->default_provider : "(empty)");
        if (strict)
            has_error = true;
    }
    if (cfg->max_tokens != 0 && (cfg->max_tokens < 1 || cfg->max_tokens > 1000000)) {
        hu_log_error("config", NULL, "max_tokens (%u) outside 1–1000000 (warning)",
                     cfg->max_tokens);
        if (strict)
            has_error = true;
    }
    if (cfg->agent.max_tool_iterations > 10000) {
        hu_log_error("config", NULL, "agent.max_tool_iterations (%u) outside 1–10000 (warning)",
                     cfg->agent.max_tool_iterations);
        if (strict)
            has_error = true;
    }

    /* URL validation */
    if (cfg->memory.api_base_url && strlen(cfg->memory.api_base_url) >= 8) {
        if (!starts_with(cfg->memory.api_base_url, "https://")) {
            hu_log_info("config", NULL, "memory.api_base_url must use https://");
            if (strict)
                has_error = true;
        }
    }
    if (cfg->diagnostics.otel_endpoint && strlen(cfg->diagnostics.otel_endpoint) >= 8) {
        if (!starts_with(cfg->diagnostics.otel_endpoint, "https://")) {
            hu_log_info("config", NULL, "diagnostics.otel_endpoint must use https://");
            if (strict)
                has_error = true;
        }
    }
    for (size_t i = 0; cfg->providers && i < cfg->providers_len; i++) {
        const char *url = cfg->providers[i].base_url;
        if (url && strlen(url) >= 8 && !starts_with(url, "https://") &&
            !starts_with(url, "http://localhost") && !starts_with(url, "http://127.0.0.1")) {
            hu_log_info("config", NULL, "providers[%zu].base_url must use https:// (or localhost)",
                        i);
            if (strict)
                has_error = true;
        }
    }

    /* Path traversal */
    if (cfg->workspace_dir && has_path_traversal(cfg->workspace_dir)) {
        hu_log_info("config", NULL, "workspace_dir must not contain '..'");
        if (strict)
            has_error = true;
    }
    if (cfg->dpo_export_dir && has_path_traversal(cfg->dpo_export_dir)) {
        hu_log_info("config", NULL, "dpo_export_dir must not contain '..'");
        if (strict)
            has_error = true;
    }
    if (cfg->memory.sqlite_path && has_path_traversal(cfg->memory.sqlite_path)) {
        hu_log_info("config", NULL, "memory.sqlite_path must not contain '..'");
        if (strict)
            has_error = true;
    }
    if (cfg->security.audit.log_path && has_path_traversal(cfg->security.audit.log_path)) {
        hu_log_info("config", NULL, "security.audit.log_path must not contain '..'");
        if (strict)
            has_error = true;
    }
    if (cfg->gateway.control_ui_dir && has_path_traversal(cfg->gateway.control_ui_dir)) {
        hu_log_info("config", NULL, "gateway.control_ui_dir must not contain '..'");
        if (strict)
            has_error = true;
    }

    check_memory_backend_build(cfg, strict, &has_error);

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
