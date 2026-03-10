#include "config_internal.h"
#include "human/config.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *default_allowed_commands[] = {"git",  "npm",  "cargo", "ls", "cat",  "grep",
                                                 "find", "echo", "pwd",   "wc", "head", "tail"};
static const size_t default_allowed_commands_len =
    sizeof(default_allowed_commands) / sizeof(default_allowed_commands[0]);

static void set_defaults(hu_config_t *cfg, hu_allocator_t *a) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->providers = NULL;
    cfg->providers_len = 0;
    cfg->api_key = NULL;
    cfg->default_provider = hu_strdup(a, "gemini");
    if (!cfg->default_provider)
        return;
    cfg->default_model = hu_strdup(a, "gemini-3.1-flash-lite-preview");
    if (!cfg->default_model)
        return;
    cfg->default_temperature = 0.7;
    cfg->temperature = 0.7;
    cfg->max_tokens = 0;
    cfg->memory_backend = hu_strdup(a, "markdown");
    if (!cfg->memory_backend)
        return;
    cfg->memory_auto_save = true;
    cfg->heartbeat_enabled = false;
    cfg->heartbeat_interval_minutes = 30;
    cfg->gateway_host = hu_strdup(a, "127.0.0.1");
    if (!cfg->gateway_host)
        return;
    cfg->gateway_port = 3000;
    cfg->workspace_only = true;
    cfg->max_actions_per_hour = 20;
    cfg->autonomy.level = hu_strdup(a, "supervised");
    if (!cfg->autonomy.level)
        return;
    cfg->autonomy.workspace_only = true;
    cfg->autonomy.max_actions_per_hour = 20;
    cfg->autonomy.require_approval_for_medium_risk = true;
    cfg->autonomy.block_high_risk_commands = true;
    cfg->autonomy.allowed_commands =
        (char **)a->alloc(a->ctx, default_allowed_commands_len * sizeof(char *));
    if (cfg->autonomy.allowed_commands) {
        for (size_t i = 0; i < default_allowed_commands_len; i++)
            cfg->autonomy.allowed_commands[i] = hu_strdup(a, default_allowed_commands[i]);
        cfg->autonomy.allowed_commands_len = default_allowed_commands_len;
    }
    cfg->autonomy.allowed_paths = NULL;
    cfg->autonomy.allowed_paths_len = 0;
    cfg->diagnostics.backend = hu_strdup(a, "none");
    if (!cfg->diagnostics.backend)
        return;
    cfg->diagnostics.otel_endpoint = NULL;
    cfg->diagnostics.otel_service_name = NULL;
    cfg->diagnostics.log_tool_calls = false;
    cfg->diagnostics.log_message_receipts = false;
    cfg->diagnostics.log_message_payloads = false;
    cfg->diagnostics.log_llm_io = false;
    cfg->agent.llm_compiler_enabled = false;
    cfg->agent.tool_routing_enabled = false;
    cfg->agent.compact_context = false;
    cfg->agent.context_pressure_warn = 0.85f;
    cfg->agent.context_pressure_compact = 0.95f;
    cfg->agent.context_compact_target = 0.70f;
    cfg->agent.max_tool_iterations = 1000;
    cfg->agent.max_history_messages = 100;
    cfg->agent.parallel_tools = false;
    cfg->agent.tool_dispatcher = hu_strdup(a, "auto");
    if (!cfg->agent.tool_dispatcher)
        return;
    cfg->agent.token_limit = HU_DEFAULT_AGENT_TOKEN_LIMIT;
    cfg->agent.session_idle_timeout_secs = 1800;
    cfg->agent.compaction_keep_recent = 20;
    cfg->agent.compaction_max_summary_chars = 2000;
    cfg->agent.compaction_max_source_chars = 12000;
    cfg->agent.message_timeout_secs = 600;
    cfg->agent.pool_max_concurrent = 8;
    cfg->agent.default_profile = NULL;
    cfg->agent.persona = NULL;
    cfg->agent.persona_channels = NULL;
    cfg->agent.persona_channels_count = 0;
    cfg->agent.persona_contacts = NULL;
    cfg->agent.persona_contacts_count = 0;
    cfg->policy.enabled = false;
    cfg->policy.rules_json = NULL;
    cfg->plugins.enabled = false;
    cfg->plugins.plugin_dir = NULL;
    cfg->plugins.plugin_paths = NULL;
    cfg->plugins.plugin_paths_len = 0;
    cfg->reliability.primary_provider = NULL;
    cfg->reliability.provider_retries = 2;
    cfg->reliability.provider_backoff_ms = 500;
    cfg->reliability.channel_initial_backoff_secs = 2;
    cfg->reliability.channel_max_backoff_secs = 60;
    cfg->reliability.scheduler_poll_secs = 15;
    cfg->reliability.scheduler_retries = 2;
    cfg->reliability.fallback_providers = NULL;
    cfg->reliability.fallback_providers_len = 0;
    cfg->router.fast = NULL;
    cfg->router.standard = NULL;
    cfg->router.powerful = NULL;
    cfg->router.complexity_low = 50;
    cfg->router.complexity_high = 500;
    cfg->runtime.kind = hu_strdup(a, "native");
    if (!cfg->runtime.kind)
        return;
    cfg->runtime.docker_image = NULL;
    cfg->runtime.gce_project = NULL;
    cfg->runtime.gce_zone = NULL;
    cfg->runtime.gce_instance = NULL;
    cfg->memory.profile = hu_strdup(a, "markdown_only");
    if (!cfg->memory.profile)
        return;
    cfg->memory.backend = hu_strdup(a, "markdown");
    if (!cfg->memory.backend)
        return;
    cfg->memory.auto_save = true;
    cfg->memory.consolidation_interval_hours = 24;
    cfg->memory.sqlite_path = NULL;
    cfg->memory.max_entries = 0;
    cfg->heartbeat.enabled = false;
    cfg->heartbeat.interval_minutes = 30;
    cfg->channels.cli = true;
    cfg->channels.default_channel = NULL;
    cfg->tunnel.provider = hu_strdup(a, "none");
    if (!cfg->tunnel.provider)
        return;
    cfg->tunnel.domain = NULL;
    cfg->gateway.enabled = true;
    cfg->gateway.port = 3000;
    cfg->gateway.host = hu_strdup(a, "127.0.0.1");
    if (!cfg->gateway.host)
        return;
    cfg->gateway.require_pairing = true;
    cfg->gateway.auth_token = NULL;
    cfg->gateway.allow_public_bind = false;
    cfg->gateway.pair_rate_limit_per_minute = 10;
    cfg->gateway.rate_limit_requests = 0;
    cfg->gateway.rate_limit_window = 0;
    cfg->gateway.webhook_hmac_secret = NULL;
    cfg->secrets.encrypt = true;
    cfg->security.sandbox = hu_strdup(a, "auto");
    if (!cfg->security.sandbox)
        return;
    cfg->security.autonomy_level = 1;
    cfg->security.sandbox_config.enabled = false;
    cfg->security.sandbox_config.backend = HU_SANDBOX_AUTO;
    cfg->security.sandbox_config.firejail_args = NULL;
    cfg->security.sandbox_config.firejail_args_len = 0;
    cfg->security.sandbox_config.net_proxy.enabled = false;
    cfg->security.sandbox_config.net_proxy.deny_all = true;
    cfg->security.sandbox_config.net_proxy.proxy_addr = NULL;
    cfg->security.sandbox_config.net_proxy.allowed_domains = NULL;
    cfg->security.sandbox_config.net_proxy.allowed_domains_len = 0;
    cfg->security.resource_limits.max_file_size = 0;
    cfg->security.resource_limits.max_read_size = 0;
    cfg->security.resource_limits.max_memory_mb = 0;
    cfg->security.audit.enabled = false;
    cfg->security.audit.log_path = NULL;
    cfg->tools.shell_timeout_secs = 60;
    cfg->tools.shell_max_output_bytes = 1048576;
    cfg->tools.max_file_size_bytes = 10485760;
    cfg->tools.web_fetch_max_chars = 100000;
    cfg->tools.web_search_provider = hu_strdup(a, "duckduckgo");
    if (!cfg->tools.web_search_provider)
        return;
    cfg->tools.enabled_tools = NULL;
    cfg->tools.enabled_tools_len = 0;
    cfg->tools.disabled_tools = NULL;
    cfg->tools.disabled_tools_len = 0;
    cfg->session.dm_scope = DirectScopePerChannelPeer;
    cfg->session.idle_minutes = 60;
    cfg->session.identity_links = NULL;
    cfg->session.identity_links_len = 0;
    cfg->identity.format = hu_strdup(a, "human");
    cfg->cost.enabled = false;
    cfg->cost.daily_limit_usd = 10.0;
    cfg->cost.monthly_limit_usd = 100.0;
    cfg->cost.warn_at_percent = 80;
    cfg->cost.allow_override = false;
    cfg->browser.enabled = false;
    cfg->peripherals.enabled = false;
    cfg->peripherals.datasheet_dir = NULL;
    cfg->hardware.enabled = false;
    cfg->hardware.transport = hu_strdup(a, "none");
    cfg->hardware.baud_rate = 115200;
    cfg->cron.enabled = false;
    cfg->cron.interval_minutes = 30;
    cfg->cron.max_run_history = 50;
    cfg->scheduler.max_concurrent = 4;
    cfg->nodes_len = 1;
    cfg->nodes[0].name = hu_strdup(a, "local");
    cfg->nodes[0].status = hu_strdup(a, "online");
}

static void sync_autonomy_level_from_string(hu_config_t *cfg) {
    if (!cfg->autonomy.level)
        return;
    if (strcmp(cfg->autonomy.level, "locked") == 0 ||
        strcmp(cfg->autonomy.level, "read_only") == 0 ||
        strcmp(cfg->autonomy.level, "readonly") == 0)
        cfg->security.autonomy_level = 0;
    else if (strcmp(cfg->autonomy.level, "supervised") == 0)
        cfg->security.autonomy_level = 1;
    else if (strcmp(cfg->autonomy.level, "assisted") == 0)
        cfg->security.autonomy_level = 2;
    else if (strcmp(cfg->autonomy.level, "autonomous") == 0 ||
             strcmp(cfg->autonomy.level, "full") == 0)
        cfg->security.autonomy_level = 3;
}

static void sync_autonomy_string_from_level(hu_config_t *cfg, hu_allocator_t *a) {
    const char *level = "supervised";
    if (cfg->security.autonomy_level == 0)
        level = "locked";
    else if (cfg->security.autonomy_level == 2)
        level = "assisted";
    else if (cfg->security.autonomy_level >= 3)
        level = "autonomous";
    if (cfg->autonomy.level)
        a->free(a->ctx, cfg->autonomy.level, strlen(cfg->autonomy.level) + 1);
    cfg->autonomy.level = hu_strdup(a, level);
}

static hu_error_t load_json_file(hu_config_t *cfg, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_OK;
    hu_allocator_t *a = &cfg->allocator;
    hu_error_t err = HU_OK;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 65536) {
        char *buf = (char *)a->alloc(a->ctx, (size_t)sz + 1);
        if (buf) {
            size_t read_len = fread(buf, 1, (size_t)sz, f);
            buf[read_len] = '\0';
            err = hu_config_parse_json(cfg, buf, read_len);
        }
    }
    fclose(f);
    return err;
}

static void sync_flat_fields(hu_config_t *cfg) {
    cfg->temperature = cfg->default_temperature;
    if (cfg->memory.backend)
        cfg->memory_backend = cfg->memory.backend;
    cfg->memory_auto_save = cfg->memory.auto_save;
    cfg->consolidation_interval_hours = cfg->memory.consolidation_interval_hours;
    cfg->heartbeat_enabled = cfg->heartbeat.enabled;
    cfg->heartbeat_interval_minutes = cfg->heartbeat.interval_minutes;
    if (cfg->gateway.host)
        cfg->gateway_host = cfg->gateway.host;
    cfg->gateway_port = cfg->gateway.port;
    cfg->workspace_only = cfg->autonomy.workspace_only;
    cfg->max_actions_per_hour = cfg->autonomy.max_actions_per_hour;
}

hu_error_t hu_config_load(hu_allocator_t *backing, hu_config_t *out) {
    if (!backing || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_arena_t *arena = hu_arena_create(*backing);
    if (!arena)
        return HU_ERR_OUT_OF_MEMORY;

    hu_allocator_t a = hu_arena_allocator(arena);
    set_defaults(out, &a);
    out->arena = arena;
    out->allocator = a;

    const char *home = getenv("HOME");
    if (!home)
        home = ".";

    char path_buf[HU_MAX_PATH];
    int n = snprintf(path_buf, sizeof(path_buf), "%s/%s/%s", home, HU_CONFIG_DIR, HU_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= sizeof(path_buf)) {
        out->config_path = hu_strdup(&a, "");
        out->workspace_dir = hu_strdup(&a, ".");
        sync_flat_fields(out);
        return hu_config_validate(out);
    }

    char global_path[HU_MAX_PATH];
    strncpy(global_path, path_buf, sizeof(global_path) - 1);
    global_path[sizeof(global_path) - 1] = '\0';

    n = snprintf(path_buf, sizeof(path_buf), "%s/%s/%s", home, HU_CONFIG_DIR, HU_DEFAULT_WORKSPACE);
    char workspace_dir[HU_MAX_PATH];
    if (n > 0 && (size_t)n < sizeof(path_buf))
        strncpy(workspace_dir, path_buf, sizeof(workspace_dir) - 1);
    else
        strncpy(workspace_dir, ".", sizeof(workspace_dir) - 1);
    workspace_dir[sizeof(workspace_dir) - 1] = '\0';

    out->config_path = hu_strdup(&a, global_path);
    out->workspace_dir = hu_strdup(&a, workspace_dir);

    hu_error_t err = load_json_file(out, global_path);
    if (err != HU_OK) {
        hu_config_deinit(out);
        return err;
    }

    /* Tighten config directory permissions if too permissive */
    {
        char dir_buf[HU_MAX_PATH];
        int dn = snprintf(dir_buf, sizeof(dir_buf), "%s/%s", home, HU_CONFIG_DIR);
        if (dn > 0 && (size_t)dn < sizeof(dir_buf)) {
            struct stat dir_st;
            if (stat(dir_buf, &dir_st) == 0 && (dir_st.st_mode & 0077) != 0)
                (void)chmod(dir_buf, 0700);
        }
    }

    char cwd[HU_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd))) {
        char workspace_cfg[HU_MAX_PATH];
        int wn = snprintf(workspace_cfg, sizeof(workspace_cfg), "%s/%s/%s", cwd, HU_CONFIG_DIR,
                          HU_CONFIG_FILE);
        if (wn > 0 && (size_t)wn < sizeof(workspace_cfg))
            load_json_file(out, workspace_cfg);
    }

    hu_config_apply_env_overrides(out);
    sync_autonomy_level_from_string(out);
    sync_flat_fields(out);
    /* Strict validation: warnings by default; fail if HUMAN_STRICT_CONFIG=1 */
    {
        const char *strict_env = getenv("HUMAN_STRICT_CONFIG");
        bool strict = (strict_env != NULL && strict_env[0] != '\0' && strict_env[0] != '0');
        hu_json_value_t *validation_root = NULL;
        if (out->config_path) {
            FILE *vf = fopen(out->config_path, "rb");
            if (vf) {
                fseek(vf, 0, SEEK_END);
                long vsz = ftell(vf);
                fseek(vf, 0, SEEK_SET);
                if (vsz > 0 && vsz < 65536) {
                    char *vbuf = (char *)a.alloc(a.ctx, (size_t)vsz + 1);
                    if (vbuf) {
                        size_t vread = fread(vbuf, 1, (size_t)vsz, vf);
                        vbuf[vread] = '\0';
                        hu_error_t verr = hu_json_parse(&a, vbuf, vread, &validation_root);
                        if (verr != HU_OK)
                            validation_root = NULL;
                    }
                }
                fclose(vf);
            }
        }
        hu_error_t verr = hu_config_validate_strict(out, validation_root, strict);
        if (verr != HU_OK)
            return verr;
    }
    return hu_config_validate(out);
}

const char *hu_config_env_get(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

void hu_config_apply_env_str(hu_allocator_t *a, char **dst, const char *v) {
    if (!v || !v[0])
        return;
    if (*dst)
        a->free(a->ctx, *dst, strlen(*dst) + 1);
    *dst = hu_strdup(a, v);
}

void hu_config_apply_env_overrides(hu_config_t *cfg) {
    if (!cfg)
        return;
    hu_allocator_t *a = &cfg->allocator;

    const char *v;
    v = getenv("HUMAN_PROVIDER");
    if (v)
        hu_config_apply_env_str(a, &cfg->default_provider, v);

    v = getenv("HUMAN_MODEL");
    if (v)
        hu_config_apply_env_str(a, &cfg->default_model, v);

    v = getenv("HUMAN_TEMPERATURE");
    if (v) {
        double temp = strtod(v, NULL);
        if (temp >= 0.0 && temp <= 2.0)
            cfg->default_temperature = temp;
    }

    v = getenv("HUMAN_GATEWAY_PORT");
    if (v) {
        unsigned long port = strtoul(v, NULL, 10);
        if (port < 1)
            port = 1;
        else if (port > 65535)
            port = 65535;
        cfg->gateway.port = (uint16_t)port;
    }

    v = getenv("HUMAN_GATEWAY_HOST");
    if (v)
        hu_config_apply_env_str(a, &cfg->gateway.host, v);

    v = getenv("HUMAN_WORKSPACE");
    if (v && !strstr(v, ".."))
        hu_config_apply_env_str(a, &cfg->workspace_dir, v);

    v = getenv("HUMAN_ALLOW_PUBLIC_BIND");
    if (v)
        cfg->gateway.allow_public_bind = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0);

    v = getenv("HUMAN_WEBHOOK_HMAC_SECRET");
    if (v)
        hu_config_apply_env_str(a, &cfg->gateway.webhook_hmac_secret, v);

    v = getenv("HUMAN_API_KEY");
    if (v)
        hu_config_apply_env_str(a, &cfg->api_key, v);
    else if (cfg->default_provider && !cfg->api_key) {
        if (strcmp(cfg->default_provider, "openai") == 0)
            v = getenv("OPENAI_API_KEY");
        else if (strcmp(cfg->default_provider, "anthropic") == 0)
            v = getenv("ANTHROPIC_API_KEY");
        else if (strcmp(cfg->default_provider, "gemini") == 0 ||
                 strcmp(cfg->default_provider, "google") == 0)
            v = getenv("GEMINI_API_KEY");
        else if (strcmp(cfg->default_provider, "ollama") == 0)
            v = getenv("OLLAMA_HOST");
        if (v)
            hu_config_apply_env_str(a, &cfg->api_key, v);
    }

    v = getenv("HUMAN_AUTONOMY");
    if (v) {
        unsigned long al = strtoul(v, NULL, 10);
        if (al <= 4)
            cfg->security.autonomy_level = (uint8_t)al;
        sync_autonomy_string_from_level(cfg, a);
    }
}
