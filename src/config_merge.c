#include "config_internal.h"
#include "seaclaw/config.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
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

static void set_defaults(sc_config_t *cfg, sc_allocator_t *a) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->providers = NULL;
    cfg->providers_len = 0;
    cfg->api_key = NULL;
    cfg->default_provider = sc_strdup(a, "gemini");
    if (!cfg->default_provider)
        return;
    cfg->default_model = sc_strdup(a, "gemini-3.1-flash-lite-preview");
    if (!cfg->default_model)
        return;
    cfg->default_temperature = 0.7;
    cfg->temperature = 0.7;
    cfg->max_tokens = 0;
    cfg->memory_backend = sc_strdup(a, "markdown");
    if (!cfg->memory_backend)
        return;
    cfg->memory_auto_save = true;
    cfg->heartbeat_enabled = false;
    cfg->heartbeat_interval_minutes = 30;
    cfg->gateway_host = sc_strdup(a, "127.0.0.1");
    if (!cfg->gateway_host)
        return;
    cfg->gateway_port = 3000;
    cfg->workspace_only = true;
    cfg->max_actions_per_hour = 20;
    cfg->autonomy.level = sc_strdup(a, "supervised");
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
            cfg->autonomy.allowed_commands[i] = sc_strdup(a, default_allowed_commands[i]);
        cfg->autonomy.allowed_commands_len = default_allowed_commands_len;
    }
    cfg->autonomy.allowed_paths = NULL;
    cfg->autonomy.allowed_paths_len = 0;
    cfg->diagnostics.backend = sc_strdup(a, "none");
    if (!cfg->diagnostics.backend)
        return;
    cfg->diagnostics.otel_endpoint = NULL;
    cfg->diagnostics.otel_service_name = NULL;
    cfg->diagnostics.log_tool_calls = false;
    cfg->diagnostics.log_message_receipts = false;
    cfg->diagnostics.log_message_payloads = false;
    cfg->diagnostics.log_llm_io = false;
    cfg->agent.compact_context = false;
    cfg->agent.max_tool_iterations = 1000;
    cfg->agent.max_history_messages = 100;
    cfg->agent.parallel_tools = false;
    cfg->agent.tool_dispatcher = sc_strdup(a, "auto");
    if (!cfg->agent.tool_dispatcher)
        return;
    cfg->agent.token_limit = SC_DEFAULT_AGENT_TOKEN_LIMIT;
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
    cfg->runtime.kind = sc_strdup(a, "native");
    if (!cfg->runtime.kind)
        return;
    cfg->runtime.docker_image = NULL;
    cfg->runtime.gce_project = NULL;
    cfg->runtime.gce_zone = NULL;
    cfg->runtime.gce_instance = NULL;
    cfg->memory.profile = sc_strdup(a, "markdown_only");
    if (!cfg->memory.profile)
        return;
    cfg->memory.backend = sc_strdup(a, "markdown");
    if (!cfg->memory.backend)
        return;
    cfg->memory.auto_save = true;
    cfg->memory.sqlite_path = NULL;
    cfg->memory.max_entries = 0;
    cfg->heartbeat.enabled = false;
    cfg->heartbeat.interval_minutes = 30;
    cfg->channels.cli = true;
    cfg->channels.default_channel = NULL;
    cfg->tunnel.provider = sc_strdup(a, "none");
    if (!cfg->tunnel.provider)
        return;
    cfg->tunnel.domain = NULL;
    cfg->gateway.enabled = true;
    cfg->gateway.port = 3000;
    cfg->gateway.host = sc_strdup(a, "127.0.0.1");
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
    cfg->security.sandbox = sc_strdup(a, "auto");
    if (!cfg->security.sandbox)
        return;
    cfg->security.autonomy_level = 1;
    cfg->security.sandbox_config.enabled = false;
    cfg->security.sandbox_config.backend = SC_SANDBOX_AUTO;
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
    cfg->tools.web_search_provider = sc_strdup(a, "duckduckgo");
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
    cfg->identity.format = sc_strdup(a, "seaclaw");
    cfg->cost.enabled = false;
    cfg->cost.daily_limit_usd = 10.0;
    cfg->cost.monthly_limit_usd = 100.0;
    cfg->cost.warn_at_percent = 80;
    cfg->cost.allow_override = false;
    cfg->browser.enabled = false;
    cfg->peripherals.enabled = false;
    cfg->peripherals.datasheet_dir = NULL;
    cfg->hardware.enabled = false;
    cfg->hardware.transport = sc_strdup(a, "none");
    cfg->hardware.baud_rate = 115200;
    cfg->cron.enabled = false;
    cfg->cron.interval_minutes = 30;
    cfg->cron.max_run_history = 50;
    cfg->scheduler.max_concurrent = 4;
    cfg->nodes_len = 1;
    cfg->nodes[0].name = sc_strdup(a, "local");
    cfg->nodes[0].status = sc_strdup(a, "online");
}

static void sync_autonomy_level_from_string(sc_config_t *cfg) {
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

static void sync_autonomy_string_from_level(sc_config_t *cfg, sc_allocator_t *a) {
    const char *level = "supervised";
    if (cfg->security.autonomy_level == 0)
        level = "locked";
    else if (cfg->security.autonomy_level == 2)
        level = "assisted";
    else if (cfg->security.autonomy_level >= 3)
        level = "autonomous";
    if (cfg->autonomy.level)
        a->free(a->ctx, cfg->autonomy.level, strlen(cfg->autonomy.level) + 1);
    cfg->autonomy.level = sc_strdup(a, level);
}

static sc_error_t load_json_file(sc_config_t *cfg, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return SC_OK;
    sc_allocator_t *a = &cfg->allocator;
    sc_error_t err = SC_OK;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz < 65536) {
        char *buf = (char *)a->alloc(a->ctx, (size_t)sz + 1);
        if (buf) {
            size_t read_len = fread(buf, 1, (size_t)sz, f);
            buf[read_len] = '\0';
            err = sc_config_parse_json(cfg, buf, read_len);
        }
    }
    fclose(f);
    return err;
}

static void sync_flat_fields(sc_config_t *cfg) {
    cfg->temperature = cfg->default_temperature;
    if (cfg->memory.backend)
        cfg->memory_backend = cfg->memory.backend;
    cfg->memory_auto_save = cfg->memory.auto_save;
    cfg->heartbeat_enabled = cfg->heartbeat.enabled;
    cfg->heartbeat_interval_minutes = cfg->heartbeat.interval_minutes;
    if (cfg->gateway.host)
        cfg->gateway_host = cfg->gateway.host;
    cfg->gateway_port = cfg->gateway.port;
    cfg->workspace_only = cfg->autonomy.workspace_only;
    cfg->max_actions_per_hour = cfg->autonomy.max_actions_per_hour;
}

sc_error_t sc_config_load(sc_allocator_t *backing, sc_config_t *out) {
    if (!backing || !out)
        return SC_ERR_INVALID_ARGUMENT;

    sc_arena_t *arena = sc_arena_create(*backing);
    if (!arena)
        return SC_ERR_OUT_OF_MEMORY;

    sc_allocator_t a = sc_arena_allocator(arena);
    set_defaults(out, &a);
    out->arena = arena;
    out->allocator = a;

    const char *home = getenv("HOME");
    if (!home)
        home = ".";

    char path_buf[SC_MAX_PATH];
    int n = snprintf(path_buf, sizeof(path_buf), "%s/%s/%s", home, SC_CONFIG_DIR, SC_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= sizeof(path_buf)) {
        out->config_path = sc_strdup(&a, "");
        out->workspace_dir = sc_strdup(&a, ".");
        sync_flat_fields(out);
        return sc_config_validate(out);
    }

    char global_path[SC_MAX_PATH];
    strncpy(global_path, path_buf, sizeof(global_path) - 1);
    global_path[sizeof(global_path) - 1] = '\0';

    n = snprintf(path_buf, sizeof(path_buf), "%s/%s/%s", home, SC_CONFIG_DIR, SC_DEFAULT_WORKSPACE);
    char workspace_dir[SC_MAX_PATH];
    if (n > 0 && (size_t)n < sizeof(path_buf))
        strncpy(workspace_dir, path_buf, sizeof(workspace_dir) - 1);
    else
        strncpy(workspace_dir, ".", sizeof(workspace_dir) - 1);
    workspace_dir[sizeof(workspace_dir) - 1] = '\0';

    out->config_path = sc_strdup(&a, global_path);
    out->workspace_dir = sc_strdup(&a, workspace_dir);

    sc_error_t err = load_json_file(out, global_path);
    if (err != SC_OK) {
        sc_config_deinit(out);
        return err;
    }

    /* Tighten config directory permissions if too permissive */
    {
        char dir_buf[SC_MAX_PATH];
        int dn = snprintf(dir_buf, sizeof(dir_buf), "%s/%s", home, SC_CONFIG_DIR);
        if (dn > 0 && (size_t)dn < sizeof(dir_buf)) {
            struct stat dir_st;
            if (stat(dir_buf, &dir_st) == 0 && (dir_st.st_mode & 0077) != 0)
                (void)chmod(dir_buf, 0700);
        }
    }

    char cwd[SC_MAX_PATH];
    if (getcwd(cwd, sizeof(cwd))) {
        char workspace_cfg[SC_MAX_PATH];
        int wn = snprintf(workspace_cfg, sizeof(workspace_cfg), "%s/%s/%s", cwd, SC_CONFIG_DIR,
                          SC_CONFIG_FILE);
        if (wn > 0 && (size_t)wn < sizeof(workspace_cfg))
            load_json_file(out, workspace_cfg);
    }

    sc_config_apply_env_overrides(out);
    sync_autonomy_level_from_string(out);
    sync_flat_fields(out);
    /* Strict validation: warnings by default; fail if SEACLAW_STRICT_CONFIG=1 */
    {
        const char *strict_env = getenv("SEACLAW_STRICT_CONFIG");
        bool strict = (strict_env != NULL && strict_env[0] != '\0' && strict_env[0] != '0');
        sc_json_value_t *validation_root = NULL;
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
                        sc_error_t verr = sc_json_parse(&a, vbuf, vread, &validation_root);
                        if (verr != SC_OK)
                            validation_root = NULL;
                    }
                }
                fclose(vf);
            }
        }
        sc_error_t verr = sc_config_validate_strict(out, validation_root, strict);
        if (verr != SC_OK)
            return verr;
    }
    return sc_config_validate(out);
}

const char *sc_config_env_get(const char *name) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

void sc_config_apply_env_str(sc_allocator_t *a, char **dst, const char *v) {
    if (!v || !v[0])
        return;
    if (*dst)
        a->free(a->ctx, *dst, strlen(*dst) + 1);
    *dst = sc_strdup(a, v);
}

void sc_config_apply_env_overrides(sc_config_t *cfg) {
    if (!cfg)
        return;
    sc_allocator_t *a = &cfg->allocator;

    const char *v;
    v = getenv("SEACLAW_PROVIDER");
    if (v)
        sc_config_apply_env_str(a, &cfg->default_provider, v);

    v = getenv("SEACLAW_MODEL");
    if (v)
        sc_config_apply_env_str(a, &cfg->default_model, v);

    v = getenv("SEACLAW_TEMPERATURE");
    if (v) {
        double temp = strtod(v, NULL);
        if (temp >= 0.0 && temp <= 2.0)
            cfg->default_temperature = temp;
    }

    v = getenv("SEACLAW_GATEWAY_PORT");
    if (v) {
        unsigned long port = strtoul(v, NULL, 10);
        if (port < 1)
            port = 1;
        else if (port > 65535)
            port = 65535;
        cfg->gateway.port = (uint16_t)port;
    }

    v = getenv("SEACLAW_GATEWAY_HOST");
    if (v)
        sc_config_apply_env_str(a, &cfg->gateway.host, v);

    v = getenv("SEACLAW_WORKSPACE");
    if (v && !strstr(v, ".."))
        sc_config_apply_env_str(a, &cfg->workspace_dir, v);

    v = getenv("SEACLAW_ALLOW_PUBLIC_BIND");
    if (v)
        cfg->gateway.allow_public_bind = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0);

    v = getenv("SEACLAW_WEBHOOK_HMAC_SECRET");
    if (v)
        sc_config_apply_env_str(a, &cfg->gateway.webhook_hmac_secret, v);

    v = getenv("SEACLAW_API_KEY");
    if (v)
        sc_config_apply_env_str(a, &cfg->api_key, v);
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
            sc_config_apply_env_str(a, &cfg->api_key, v);
    }

    v = getenv("SEACLAW_AUTONOMY");
    if (v) {
        unsigned long al = strtoul(v, NULL, 10);
        if (al <= 4)
            cfg->security.autonomy_level = (uint8_t)al;
        sync_autonomy_string_from_level(cfg, a);
    }
}
