#include "seaclaw/config.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config_internal.h"

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
    cfg->default_model = sc_strdup(a, "gemini-3.1-flash-lite-preview");
    cfg->default_temperature = 0.7;
    cfg->temperature = 0.7;
    cfg->max_tokens = 0;
    cfg->memory_backend = sc_strdup(a, "markdown");
    cfg->memory_auto_save = true;
    cfg->heartbeat_enabled = false;
    cfg->heartbeat_interval_minutes = 30;
    cfg->gateway_host = sc_strdup(a, "127.0.0.1");
    cfg->gateway_port = 3000;
    cfg->workspace_only = true;
    cfg->max_actions_per_hour = 20;
    cfg->autonomy.level = sc_strdup(a, "supervised");
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
    cfg->agent.token_limit = SC_DEFAULT_AGENT_TOKEN_LIMIT;
    cfg->agent.session_idle_timeout_secs = 1800;
    cfg->agent.compaction_keep_recent = 20;
    cfg->agent.compaction_max_summary_chars = 2000;
    cfg->agent.compaction_max_source_chars = 12000;
    cfg->agent.message_timeout_secs = 600;
    cfg->agent.pool_max_concurrent = 8;
    cfg->agent.default_profile = NULL;
    cfg->agent.persona = NULL;
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
    cfg->runtime.docker_image = NULL;
    cfg->runtime.gce_project = NULL;
    cfg->runtime.gce_zone = NULL;
    cfg->runtime.gce_instance = NULL;
    cfg->memory.profile = sc_strdup(a, "markdown_only");
    cfg->memory.backend = sc_strdup(a, "markdown");
    cfg->memory.auto_save = true;
    cfg->memory.sqlite_path = NULL;
    cfg->memory.max_entries = 0;
    cfg->heartbeat.enabled = false;
    cfg->heartbeat.interval_minutes = 30;
    cfg->channels.cli = true;
    cfg->channels.default_channel = NULL;
    cfg->tunnel.provider = sc_strdup(a, "none");
    cfg->tunnel.domain = NULL;
    cfg->gateway.enabled = true;
    cfg->gateway.port = 3000;
    cfg->gateway.host = sc_strdup(a, "127.0.0.1");
    cfg->gateway.require_pairing = true;
    cfg->gateway.auth_token = NULL;
    cfg->gateway.allow_public_bind = false;
    cfg->gateway.pair_rate_limit_per_minute = 10;
    cfg->gateway.rate_limit_requests = 0;
    cfg->gateway.rate_limit_window = 0;
    cfg->gateway.webhook_hmac_secret = NULL;
    cfg->secrets.encrypt = true;
    cfg->security.sandbox = sc_strdup(a, "auto");
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

static sc_error_t parse_nodes(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *arr) {
    if (!arr || arr->type != SC_JSON_ARRAY)
        return SC_OK;
    for (size_t i = 0; i < cfg->nodes_len; i++) {
        if (cfg->nodes[i].name) {
            a->free(a->ctx, cfg->nodes[i].name, strlen(cfg->nodes[i].name) + 1);
            cfg->nodes[i].name = NULL;
        }
        if (cfg->nodes[i].status) {
            a->free(a->ctx, cfg->nodes[i].status, strlen(cfg->nodes[i].status) + 1);
            cfg->nodes[i].status = NULL;
        }
    }
    cfg->nodes_len = 0;
    size_t cap = arr->data.array.len;
    if (cap > SC_NODES_MAX)
        cap = SC_NODES_MAX;
    size_t n = 0;
    for (size_t i = 0; i < cap; i++) {
        const sc_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;
        const char *name = sc_json_get_string(item, "name");
        if (!name)
            continue;
        const char *status = sc_json_get_string(item, "status");
        cfg->nodes[n].name = sc_strdup(a, name);
        cfg->nodes[n].status = status && status[0] ? sc_strdup(a, status) : sc_strdup(a, "online");
        n++;
    }
    cfg->nodes_len = n;
    return SC_OK;
}

static sc_error_t parse_providers(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *arr) {
    if (!arr || arr->type != SC_JSON_ARRAY)
        return SC_OK;
    size_t cap = arr->data.array.len;
    if (cap == 0)
        return SC_OK;

    sc_provider_entry_t *providers =
        (sc_provider_entry_t *)a->alloc(a->ctx, cap * sizeof(sc_provider_entry_t));
    if (!providers)
        return SC_ERR_OUT_OF_MEMORY;
    memset(providers, 0, cap * sizeof(sc_provider_entry_t));

    size_t n = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        const sc_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;

        const char *name = sc_json_get_string(item, "name");
        if (!name)
            continue;

        providers[n].name = sc_strdup(a, name);
        const char *api_key = sc_json_get_string(item, "api_key");
        if (api_key)
            providers[n].api_key = sc_strdup(a, api_key);
        const char *base_url = sc_json_get_string(item, "base_url");
        if (base_url)
            providers[n].base_url = sc_strdup(a, base_url);
        providers[n].native_tools = sc_json_get_bool(item, "native_tools", true);
        providers[n].ws_streaming = sc_json_get_bool(item, "ws_streaming", false);

        if (providers[n].name)
            n++;
    }
    cfg->providers = providers;
    cfg->providers_len = n;
    return SC_OK;
}

static sc_error_t parse_string_array(sc_allocator_t *a, char ***out, size_t *out_len,
                                     const sc_json_value_t *arr) {
    if (!arr || arr->type != SC_JSON_ARRAY)
        return SC_OK;
    size_t n = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        if (arr->data.array.items[i] && arr->data.array.items[i]->type == SC_JSON_STRING)
            n++;
    }
    if (n == 0)
        return SC_OK;

    char **list = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!list)
        return SC_ERR_OUT_OF_MEMORY;

    size_t j = 0;
    for (size_t i = 0; i < arr->data.array.len && j < n; i++) {
        const sc_json_value_t *v = arr->data.array.items[i];
        if (!v || v->type != SC_JSON_STRING)
            continue;
        const char *s = v->data.string.ptr;
        if (s)
            list[j++] = sc_strdup(a, s);
    }
    *out = list;
    *out_len = j;
    return SC_OK;
}

static sc_error_t parse_autonomy(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;

    const char *level = sc_json_get_string(obj, "level");
    if (level) {
        if (cfg->autonomy.level)
            a->free(a->ctx, cfg->autonomy.level, strlen(cfg->autonomy.level) + 1);
        cfg->autonomy.level = sc_strdup(a, level);
    }
    cfg->autonomy.workspace_only =
        sc_json_get_bool(obj, "workspace_only", cfg->autonomy.workspace_only);
    double max_act =
        sc_json_get_number(obj, "max_actions_per_hour", cfg->autonomy.max_actions_per_hour);
    if (max_act >= 0 && max_act <= 1000000)
        cfg->autonomy.max_actions_per_hour = (uint32_t)max_act;
    cfg->autonomy.require_approval_for_medium_risk = sc_json_get_bool(
        obj, "require_approval_for_medium_risk", cfg->autonomy.require_approval_for_medium_risk);
    cfg->autonomy.block_high_risk_commands =
        sc_json_get_bool(obj, "block_high_risk_commands", cfg->autonomy.block_high_risk_commands);

    sc_json_value_t *ac = sc_json_object_get(obj, "allowed_commands");
    if (ac && ac->type == SC_JSON_ARRAY) {
        if (cfg->autonomy.allowed_commands) {
            for (size_t i = 0; i < cfg->autonomy.allowed_commands_len; i++)
                a->free(a->ctx, cfg->autonomy.allowed_commands[i],
                        strlen(cfg->autonomy.allowed_commands[i]) + 1);
            a->free(a->ctx, cfg->autonomy.allowed_commands,
                    cfg->autonomy.allowed_commands_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->autonomy.allowed_commands, &cfg->autonomy.allowed_commands_len,
                           ac);
    }
    return SC_OK;
}

const char *sc_config_sandbox_backend_to_string(sc_sandbox_backend_t b) {
    switch (b) {
    case SC_SANDBOX_AUTO:
        return "auto";
    case SC_SANDBOX_NONE:
        return "none";
    case SC_SANDBOX_LANDLOCK:
        return "landlock";
    case SC_SANDBOX_FIREJAIL:
        return "firejail";
    case SC_SANDBOX_BUBBLEWRAP:
        return "bubblewrap";
    case SC_SANDBOX_DOCKER:
        return "docker";
    case SC_SANDBOX_SEATBELT:
        return "seatbelt";
    case SC_SANDBOX_SECCOMP:
        return "seccomp";
    case SC_SANDBOX_LANDLOCK_SECCOMP:
        return "landlock_seccomp";
    case SC_SANDBOX_WASI:
        return "wasi";
    case SC_SANDBOX_FIRECRACKER:
        return "firecracker";
    case SC_SANDBOX_APPCONTAINER:
        return "appcontainer";
    }
    return "auto";
}

static sc_sandbox_backend_t parse_sandbox_backend(const char *s) {
    if (!s)
        return SC_SANDBOX_AUTO;
    if (strcmp(s, "landlock") == 0)
        return SC_SANDBOX_LANDLOCK;
    if (strcmp(s, "firejail") == 0)
        return SC_SANDBOX_FIREJAIL;
    if (strcmp(s, "bubblewrap") == 0)
        return SC_SANDBOX_BUBBLEWRAP;
    if (strcmp(s, "docker") == 0)
        return SC_SANDBOX_DOCKER;
    if (strcmp(s, "seatbelt") == 0)
        return SC_SANDBOX_SEATBELT;
    if (strcmp(s, "seccomp") == 0)
        return SC_SANDBOX_SECCOMP;
    if (strcmp(s, "landlock+seccomp") == 0)
        return SC_SANDBOX_LANDLOCK_SECCOMP;
    if (strcmp(s, "wasi") == 0)
        return SC_SANDBOX_WASI;
    if (strcmp(s, "firecracker") == 0)
        return SC_SANDBOX_FIRECRACKER;
    if (strcmp(s, "appcontainer") == 0)
        return SC_SANDBOX_APPCONTAINER;
    if (strcmp(s, "none") == 0)
        return SC_SANDBOX_NONE;
    return SC_SANDBOX_AUTO;
}

static sc_error_t parse_cron(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->cron.enabled = sc_json_get_bool(obj, "enabled", cfg->cron.enabled);
    double im = sc_json_get_number(obj, "interval_minutes", cfg->cron.interval_minutes);
    if (im >= 1 && im <= 1440)
        cfg->cron.interval_minutes = (uint32_t)im;
    double mrh = sc_json_get_number(obj, "max_run_history", cfg->cron.max_run_history);
    if (mrh >= 0 && mrh <= 10000)
        cfg->cron.max_run_history = (uint32_t)mrh;
    return SC_OK;
}

static sc_error_t parse_scheduler(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    double mc = sc_json_get_number(obj, "max_concurrent", cfg->scheduler.max_concurrent);
    if (mc >= 0 && mc <= 256)
        cfg->scheduler.max_concurrent = (uint32_t)mc;
    return SC_OK;
}

static sc_error_t parse_gateway(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->gateway.enabled = sc_json_get_bool(obj, "enabled", cfg->gateway.enabled);
    double port = sc_json_get_number(obj, "port", cfg->gateway.port);
    if (port < 1)
        port = 1;
    else if (port > 65535)
        port = 65535;
    cfg->gateway.port = (uint16_t)port;
    const char *host = sc_json_get_string(obj, "host");
    if (host) {
        if (cfg->gateway.host)
            a->free(a->ctx, cfg->gateway.host, strlen(cfg->gateway.host) + 1);
        cfg->gateway.host = sc_strdup(a, host);
    }
    cfg->gateway.require_pairing =
        sc_json_get_bool(obj, "require_pairing", cfg->gateway.require_pairing);
    const char *at = sc_json_get_string(obj, "auth_token");
    if (at) {
        if (cfg->gateway.auth_token)
            a->free(a->ctx, cfg->gateway.auth_token, strlen(cfg->gateway.auth_token) + 1);
        cfg->gateway.auth_token = sc_strdup(a, at);
    }
    cfg->gateway.allow_public_bind =
        sc_json_get_bool(obj, "allow_public_bind", cfg->gateway.allow_public_bind);
    double prl = sc_json_get_number(obj, "pair_rate_limit_per_minute",
                                    cfg->gateway.pair_rate_limit_per_minute);
    if (prl >= 0 && prl <= 1000)
        cfg->gateway.pair_rate_limit_per_minute = (uint32_t)prl;
    double rlr = sc_json_get_number(obj, "rate_limit_requests", cfg->gateway.rate_limit_requests);
    if (rlr >= 0 && rlr <= 10000)
        cfg->gateway.rate_limit_requests = (int)rlr;
    double rlw = sc_json_get_number(obj, "rate_limit_window", cfg->gateway.rate_limit_window);
    if (rlw >= 0 && rlw <= 86400)
        cfg->gateway.rate_limit_window = (int)rlw;
    const char *whs = sc_json_get_string(obj, "webhook_hmac_secret");
    if (whs) {
        if (cfg->gateway.webhook_hmac_secret)
            a->free(a->ctx, cfg->gateway.webhook_hmac_secret,
                    strlen(cfg->gateway.webhook_hmac_secret) + 1);
        cfg->gateway.webhook_hmac_secret = sc_strdup(a, whs);
    }
    const char *uid = sc_json_get_string(obj, "control_ui_dir");
    if (uid) {
        if (cfg->gateway.control_ui_dir)
            a->free(a->ctx, cfg->gateway.control_ui_dir, strlen(cfg->gateway.control_ui_dir) + 1);
        cfg->gateway.control_ui_dir = sc_strdup(a, uid);
    }
    sc_json_value_t *cors = sc_json_object_get(obj, "cors_origins");
    if (cors && cors->type == SC_JSON_ARRAY) {
        if (cfg->gateway.cors_origins) {
            for (size_t i = 0; i < cfg->gateway.cors_origins_len; i++)
                if (cfg->gateway.cors_origins[i])
                    a->free(a->ctx, cfg->gateway.cors_origins[i],
                            strlen(cfg->gateway.cors_origins[i]) + 1);
            a->free(a->ctx, cfg->gateway.cors_origins,
                    cfg->gateway.cors_origins_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->gateway.cors_origins, &cfg->gateway.cors_origins_len, cors);
    }
    return SC_OK;
}

static sc_error_t parse_memory(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    const char *profile = sc_json_get_string(obj, "profile");
    if (profile) {
        if (cfg->memory.profile)
            a->free(a->ctx, cfg->memory.profile, strlen(cfg->memory.profile) + 1);
        cfg->memory.profile = sc_strdup(a, profile);
    }
    const char *backend = sc_json_get_string(obj, "backend");
    if (backend) {
        if (cfg->memory.backend)
            a->free(a->ctx, cfg->memory.backend, strlen(cfg->memory.backend) + 1);
        cfg->memory.backend = sc_strdup(a, backend);
    }
    const char *sqlite_path = sc_json_get_string(obj, "sqlite_path");
    if (sqlite_path) {
        if (cfg->memory.sqlite_path)
            a->free(a->ctx, cfg->memory.sqlite_path, strlen(cfg->memory.sqlite_path) + 1);
        cfg->memory.sqlite_path = sc_strdup(a, sqlite_path);
    }
    double max_ent = sc_json_get_number(obj, "max_entries", cfg->memory.max_entries);
    if (max_ent >= 0 && max_ent <= 1000000)
        cfg->memory.max_entries = (uint32_t)max_ent;
    cfg->memory.auto_save = sc_json_get_bool(obj, "auto_save", cfg->memory.auto_save);

    const char *pg_url = sc_json_get_string(obj, "postgres_url");
    if (pg_url) {
        if (cfg->memory.postgres_url)
            a->free(a->ctx, cfg->memory.postgres_url, strlen(cfg->memory.postgres_url) + 1);
        cfg->memory.postgres_url = sc_strdup(a, pg_url);
    }
    const char *pg_schema = sc_json_get_string(obj, "postgres_schema");
    if (pg_schema) {
        if (cfg->memory.postgres_schema)
            a->free(a->ctx, cfg->memory.postgres_schema, strlen(cfg->memory.postgres_schema) + 1);
        cfg->memory.postgres_schema = sc_strdup(a, pg_schema);
    }
    const char *pg_table = sc_json_get_string(obj, "postgres_table");
    if (pg_table) {
        if (cfg->memory.postgres_table)
            a->free(a->ctx, cfg->memory.postgres_table, strlen(cfg->memory.postgres_table) + 1);
        cfg->memory.postgres_table = sc_strdup(a, pg_table);
    }
    const char *r_host = sc_json_get_string(obj, "redis_host");
    if (r_host) {
        if (cfg->memory.redis_host)
            a->free(a->ctx, cfg->memory.redis_host, strlen(cfg->memory.redis_host) + 1);
        cfg->memory.redis_host = sc_strdup(a, r_host);
    }
    double r_port = sc_json_get_number(obj, "redis_port", cfg->memory.redis_port);
    if (r_port >= 1 && r_port <= 65535)
        cfg->memory.redis_port = (uint16_t)r_port;
    const char *r_prefix = sc_json_get_string(obj, "redis_key_prefix");
    if (r_prefix) {
        if (cfg->memory.redis_key_prefix)
            a->free(a->ctx, cfg->memory.redis_key_prefix, strlen(cfg->memory.redis_key_prefix) + 1);
        cfg->memory.redis_key_prefix = sc_strdup(a, r_prefix);
    }
    const char *api_url = sc_json_get_string(obj, "api_base_url");
    if (api_url) {
        if (cfg->memory.api_base_url)
            a->free(a->ctx, cfg->memory.api_base_url, strlen(cfg->memory.api_base_url) + 1);
        cfg->memory.api_base_url = sc_strdup(a, api_url);
    }
    const char *api_k = sc_json_get_string(obj, "api_key");
    if (api_k) {
        if (cfg->memory.api_key)
            a->free(a->ctx, cfg->memory.api_key, strlen(cfg->memory.api_key) + 1);
        cfg->memory.api_key = sc_strdup(a, api_k);
    }
    double api_tm = sc_json_get_number(obj, "api_timeout_ms", cfg->memory.api_timeout_ms);
    if (api_tm >= 0 && api_tm <= 300000)
        cfg->memory.api_timeout_ms = (uint32_t)api_tm;
    return SC_OK;
}

static sc_error_t parse_tools(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    double sts = sc_json_get_number(obj, "shell_timeout_secs", cfg->tools.shell_timeout_secs);
    if (sts >= 1 && sts <= 86400)
        cfg->tools.shell_timeout_secs = (uint64_t)sts;
    double smob =
        sc_json_get_number(obj, "shell_max_output_bytes", cfg->tools.shell_max_output_bytes);
    if (smob >= 0 && smob <= 1073741824)
        cfg->tools.shell_max_output_bytes = (uint32_t)smob;
    double mfsb = sc_json_get_number(obj, "max_file_size_bytes", cfg->tools.max_file_size_bytes);
    if (mfsb >= 0 && mfsb <= 1073741824)
        cfg->tools.max_file_size_bytes = (uint32_t)mfsb;
    double wfmc = sc_json_get_number(obj, "web_fetch_max_chars", cfg->tools.web_fetch_max_chars);
    if (wfmc >= 0 && wfmc <= 10000000)
        cfg->tools.web_fetch_max_chars = (uint32_t)wfmc;
    const char *provider = sc_json_get_string(obj, "web_search_provider");
    if (provider && provider[0]) {
        if (cfg->tools.web_search_provider)
            a->free(a->ctx, cfg->tools.web_search_provider,
                    strlen(cfg->tools.web_search_provider) + 1);
        cfg->tools.web_search_provider = sc_strdup(a, provider);
    }
    sc_json_value_t *en = sc_json_object_get(obj, "enabled_tools");
    if (en && en->type == SC_JSON_ARRAY) {
        if (cfg->tools.enabled_tools) {
            for (size_t i = 0; i < cfg->tools.enabled_tools_len; i++)
                a->free(a->ctx, cfg->tools.enabled_tools[i],
                        strlen(cfg->tools.enabled_tools[i]) + 1);
            a->free(a->ctx, cfg->tools.enabled_tools,
                    cfg->tools.enabled_tools_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->tools.enabled_tools, &cfg->tools.enabled_tools_len, en);
    }
    sc_json_value_t *dis = sc_json_object_get(obj, "disabled_tools");
    if (dis && dis->type == SC_JSON_ARRAY) {
        if (cfg->tools.disabled_tools) {
            for (size_t i = 0; i < cfg->tools.disabled_tools_len; i++)
                a->free(a->ctx, cfg->tools.disabled_tools[i],
                        strlen(cfg->tools.disabled_tools[i]) + 1);
            a->free(a->ctx, cfg->tools.disabled_tools,
                    cfg->tools.disabled_tools_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->tools.disabled_tools, &cfg->tools.disabled_tools_len, dis);
    }
    return SC_OK;
}

static sc_error_t parse_runtime(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    const char *kind = sc_json_get_string(obj, "kind");
    if (!kind)
        kind = sc_json_get_string(obj, "type");
    if (kind) {
        if (cfg->runtime.kind)
            a->free(a->ctx, cfg->runtime.kind, strlen(cfg->runtime.kind) + 1);
        cfg->runtime.kind = sc_strdup(a, kind);
    }
    const char *docker_image = sc_json_get_string(obj, "docker_image");
    if (docker_image) {
        if (cfg->runtime.docker_image)
            a->free(a->ctx, cfg->runtime.docker_image, strlen(cfg->runtime.docker_image) + 1);
        cfg->runtime.docker_image = sc_strdup(a, docker_image);
    }
    const char *gce_project = sc_json_get_string(obj, "gce_project");
    if (gce_project) {
        if (cfg->runtime.gce_project)
            a->free(a->ctx, cfg->runtime.gce_project, strlen(cfg->runtime.gce_project) + 1);
        cfg->runtime.gce_project = sc_strdup(a, gce_project);
    }
    const char *gce_zone = sc_json_get_string(obj, "gce_zone");
    if (gce_zone) {
        if (cfg->runtime.gce_zone)
            a->free(a->ctx, cfg->runtime.gce_zone, strlen(cfg->runtime.gce_zone) + 1);
        cfg->runtime.gce_zone = sc_strdup(a, gce_zone);
    }
    const char *gce_instance = sc_json_get_string(obj, "gce_instance");
    if (gce_instance) {
        if (cfg->runtime.gce_instance)
            a->free(a->ctx, cfg->runtime.gce_instance, strlen(cfg->runtime.gce_instance) + 1);
        cfg->runtime.gce_instance = sc_strdup(a, gce_instance);
    }
    return SC_OK;
}

static sc_error_t parse_tunnel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    const char *provider = sc_json_get_string(obj, "provider");
    if (provider) {
        if (cfg->tunnel.provider)
            a->free(a->ctx, cfg->tunnel.provider, strlen(cfg->tunnel.provider) + 1);
        cfg->tunnel.provider = sc_strdup(a, provider);
    }
    const char *domain = sc_json_get_string(obj, "domain");
    if (domain) {
        if (cfg->tunnel.domain)
            a->free(a->ctx, cfg->tunnel.domain, strlen(cfg->tunnel.domain) + 1);
        cfg->tunnel.domain = sc_strdup(a, domain);
    }
    return SC_OK;
}

static void parse_email_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    sc_email_channel_config_t *e = &cfg->channels.email;
    const char *s;
    s = sc_json_get_string(obj, "smtp_host");
    if (s) {
        if (e->smtp_host)
            a->free(a->ctx, e->smtp_host, strlen(e->smtp_host) + 1);
        e->smtp_host = sc_strdup(a, s);
    }
    double port = sc_json_get_number(obj, "smtp_port", e->smtp_port);
    if (port >= 1 && port <= 65535)
        e->smtp_port = (uint16_t)port;
    s = sc_json_get_string(obj, "from_address");
    if (s) {
        if (e->from_address)
            a->free(a->ctx, e->from_address, strlen(e->from_address) + 1);
        e->from_address = sc_strdup(a, s);
    }
    s = sc_json_get_string(obj, "smtp_user");
    if (s) {
        if (e->smtp_user)
            a->free(a->ctx, e->smtp_user, strlen(e->smtp_user) + 1);
        e->smtp_user = sc_strdup(a, s);
    }
    s = sc_json_get_string(obj, "smtp_pass");
    if (s) {
        if (e->smtp_pass)
            a->free(a->ctx, e->smtp_pass, strlen(e->smtp_pass) + 1);
        e->smtp_pass = sc_strdup(a, s);
    }
    s = sc_json_get_string(obj, "imap_host");
    if (s) {
        if (e->imap_host)
            a->free(a->ctx, e->imap_host, strlen(e->imap_host) + 1);
        e->imap_host = sc_strdup(a, s);
    }
    port = sc_json_get_number(obj, "imap_port", e->imap_port);
    if (port >= 1 && port <= 65535)
        e->imap_port = (uint16_t)port;
}

static void parse_imap_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    sc_imap_channel_config_t *im = &cfg->channels.imap;
    const char *val;
    val = sc_json_get_string(obj, "imap_host");
    if (val) {
        if (im->imap_host)
            a->free(a->ctx, im->imap_host, strlen(im->imap_host) + 1);
        im->imap_host = sc_strdup(a, val);
    }
    double port = sc_json_get_number(obj, "imap_port", im->imap_port);
    if (port >= 1 && port <= 65535)
        im->imap_port = (uint16_t)port;
    val = sc_json_get_string(obj, "imap_username");
    if (val) {
        if (im->imap_username)
            a->free(a->ctx, im->imap_username, strlen(im->imap_username) + 1);
        im->imap_username = sc_strdup(a, val);
    }
    val = sc_json_get_string(obj, "imap_password");
    if (val) {
        if (im->imap_password)
            a->free(a->ctx, im->imap_password, strlen(im->imap_password) + 1);
        im->imap_password = sc_strdup(a, val);
    }
    val = sc_json_get_string(obj, "imap_folder");
    if (val) {
        if (im->imap_folder)
            a->free(a->ctx, im->imap_folder, strlen(im->imap_folder) + 1);
        im->imap_folder = sc_strdup(a, val);
    }
    im->imap_use_tls = sc_json_get_bool(obj, "imap_use_tls", true);
}

static void parse_imessage_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *t = sc_json_get_string(obj, "default_target");
    if (t) {
        if (cfg->channels.imessage.default_target)
            a->free(a->ctx, cfg->channels.imessage.default_target,
                    strlen(cfg->channels.imessage.default_target) + 1);
        cfg->channels.imessage.default_target = sc_strdup(a, t);
    }
    sc_json_value_t *af = sc_json_object_get(obj, "allow_from");
    if (af && af->type == SC_JSON_ARRAY) {
        if (cfg->channels.imessage.allow_from) {
            for (size_t i = 0; i < cfg->channels.imessage.allow_from_count; i++)
                if (cfg->channels.imessage.allow_from[i])
                    a->free(a->ctx, cfg->channels.imessage.allow_from[i],
                            strlen(cfg->channels.imessage.allow_from[i]) + 1);
            a->free(a->ctx, cfg->channels.imessage.allow_from,
                    cfg->channels.imessage.allow_from_count * sizeof(char *));
        }
        parse_string_array(a, &cfg->channels.imessage.allow_from,
                           &cfg->channels.imessage.allow_from_count, af);
    }
    cfg->channels.imessage.poll_interval_sec =
        (int)sc_json_get_number(obj, "poll_interval_sec", 30.0);
}

static void parse_gmail_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return;
    const char *cid = sc_json_get_string(obj, "client_id");
    const char *csec = sc_json_get_string(obj, "client_secret");
    const char *rtok = sc_json_get_string(obj, "refresh_token");
    if (cid) {
        if (cfg->channels.gmail.client_id)
            a->free(a->ctx, cfg->channels.gmail.client_id,
                    strlen(cfg->channels.gmail.client_id) + 1);
        cfg->channels.gmail.client_id = sc_strndup(a, cid, strlen(cid));
    }
    if (csec) {
        if (cfg->channels.gmail.client_secret)
            a->free(a->ctx, cfg->channels.gmail.client_secret,
                    strlen(cfg->channels.gmail.client_secret) + 1);
        cfg->channels.gmail.client_secret = sc_strndup(a, csec, strlen(csec));
    }
    if (rtok) {
        if (cfg->channels.gmail.refresh_token)
            a->free(a->ctx, cfg->channels.gmail.refresh_token,
                    strlen(cfg->channels.gmail.refresh_token) + 1);
        cfg->channels.gmail.refresh_token = sc_strndup(a, rtok, strlen(rtok));
    }
    cfg->channels.gmail.poll_interval_sec = (int)sc_json_get_number(obj, "poll_interval_sec", 30.0);
}

static void parse_telegram_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_telegram_channel_config_t *t = &cfg->channels.telegram;

    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;

    const char *s = sc_json_get_string(val, "token");
    if (s) {
        if (t->token)
            a->free(a->ctx, t->token, strlen(t->token) + 1);
        t->token = sc_strdup(a, s);
    }

    sc_json_value_t *af = sc_json_object_get(val, "allow_from");
    if (af && af->type == SC_JSON_ARRAY && af->data.array.items) {
        for (size_t i = 0; i < t->allow_from_count; i++) {
            if (t->allow_from[i])
                a->free(a->ctx, t->allow_from[i], strlen(t->allow_from[i]) + 1);
        }
        t->allow_from_count = 0;
        for (size_t i = 0;
             i < af->data.array.len && t->allow_from_count < SC_TELEGRAM_ALLOW_FROM_MAX; i++) {
            sc_json_value_t *item = af->data.array.items[i];
            if (item && item->type == SC_JSON_STRING && item->data.string.ptr)
                t->allow_from[t->allow_from_count++] = sc_strdup(a, item->data.string.ptr);
        }
    }
}

static void parse_discord_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_discord_channel_config_t *d = &cfg->channels.discord;

    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;

    const char *s = sc_json_get_string(val, "token");
    if (s) {
        if (d->token)
            a->free(a->ctx, d->token, strlen(d->token) + 1);
        d->token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "guild_id");
    if (s) {
        if (d->guild_id)
            a->free(a->ctx, d->guild_id, strlen(d->guild_id) + 1);
        d->guild_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "bot_id");
    if (s) {
        if (d->bot_id)
            a->free(a->ctx, d->bot_id, strlen(d->bot_id) + 1);
        d->bot_id = sc_strdup(a, s);
    }

    sc_json_value_t *ch_ids = sc_json_object_get(val, "channel_ids");
    if (ch_ids && ch_ids->type == SC_JSON_ARRAY && ch_ids->data.array.items) {
        for (size_t i = 0; i < d->channel_ids_count; i++) {
            if (d->channel_ids[i])
                a->free(a->ctx, d->channel_ids[i], strlen(d->channel_ids[i]) + 1);
        }
        d->channel_ids_count = 0;
        for (size_t i = 0;
             i < ch_ids->data.array.len && d->channel_ids_count < SC_DISCORD_CHANNEL_IDS_MAX; i++) {
            sc_json_value_t *item = ch_ids->data.array.items[i];
            if (item && item->type == SC_JSON_STRING && item->data.string.ptr) {
                d->channel_ids[d->channel_ids_count++] = sc_strdup(a, item->data.string.ptr);
            }
        }
    }
}

static void parse_slack_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_slack_channel_config_t *sl = &cfg->channels.slack;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "token");
    if (s) {
        if (sl->token)
            a->free(a->ctx, sl->token, strlen(sl->token) + 1);
        sl->token = sc_strdup(a, s);
    }
    sc_json_value_t *ch_ids = sc_json_object_get(val, "channel_ids");
    if (ch_ids && ch_ids->type == SC_JSON_ARRAY && ch_ids->data.array.items) {
        for (size_t i = 0; i < sl->channel_ids_count; i++)
            if (sl->channel_ids[i])
                a->free(a->ctx, sl->channel_ids[i], strlen(sl->channel_ids[i]) + 1);
        sl->channel_ids_count = 0;
        for (size_t i = 0;
             i < ch_ids->data.array.len && sl->channel_ids_count < SC_SLACK_CHANNEL_IDS_MAX; i++) {
            sc_json_value_t *item = ch_ids->data.array.items[i];
            if (item && item->type == SC_JSON_STRING && item->data.string.ptr)
                sl->channel_ids[sl->channel_ids_count++] = sc_strdup(a, item->data.string.ptr);
        }
    }
}

static void parse_whatsapp_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_whatsapp_channel_config_t *wa = &cfg->channels.whatsapp;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "phone_number_id");
    if (s) {
        if (wa->phone_number_id)
            a->free(a->ctx, wa->phone_number_id, strlen(wa->phone_number_id) + 1);
        wa->phone_number_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "token");
    if (s) {
        if (wa->token)
            a->free(a->ctx, wa->token, strlen(wa->token) + 1);
        wa->token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "verify_token");
    if (s) {
        if (wa->verify_token)
            a->free(a->ctx, wa->verify_token, strlen(wa->verify_token) + 1);
        wa->verify_token = sc_strdup(a, s);
    }
}

static void parse_line_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_line_channel_config_t *ln = &cfg->channels.line;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "channel_token");
    if (s) {
        if (ln->channel_token)
            a->free(a->ctx, ln->channel_token, strlen(ln->channel_token) + 1);
        ln->channel_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "channel_secret");
    if (s) {
        if (ln->channel_secret)
            a->free(a->ctx, ln->channel_secret, strlen(ln->channel_secret) + 1);
        ln->channel_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "user_id");
    if (s) {
        if (ln->user_id)
            a->free(a->ctx, ln->user_id, strlen(ln->user_id) + 1);
        ln->user_id = sc_strdup(a, s);
    }
}

static void parse_google_chat_channel(sc_allocator_t *a, sc_config_t *cfg,
                                      const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_google_chat_channel_config_t *gc = &cfg->channels.google_chat;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "webhook_url");
    if (s) {
        if (gc->webhook_url)
            a->free(a->ctx, gc->webhook_url, strlen(gc->webhook_url) + 1);
        gc->webhook_url = sc_strdup(a, s);
    }
}

static void parse_facebook_channel(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_facebook_channel_config_t *fb = &cfg->channels.facebook;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "page_id");
    if (s) {
        if (fb->page_id)
            a->free(a->ctx, fb->page_id, strlen(fb->page_id) + 1);
        fb->page_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "page_access_token");
    if (s) {
        if (fb->page_access_token)
            a->free(a->ctx, fb->page_access_token, strlen(fb->page_access_token) + 1);
        fb->page_access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "verify_token");
    if (s) {
        if (fb->verify_token)
            a->free(a->ctx, fb->verify_token, strlen(fb->verify_token) + 1);
        fb->verify_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "app_secret");
    if (s) {
        if (fb->app_secret)
            a->free(a->ctx, fb->app_secret, strlen(fb->app_secret) + 1);
        fb->app_secret = sc_strdup(a, s);
    }
}

static void parse_instagram_channel(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_instagram_channel_config_t *ig = &cfg->channels.instagram;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "business_account_id");
    if (s) {
        if (ig->business_account_id)
            a->free(a->ctx, ig->business_account_id, strlen(ig->business_account_id) + 1);
        ig->business_account_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token");
    if (s) {
        if (ig->access_token)
            a->free(a->ctx, ig->access_token, strlen(ig->access_token) + 1);
        ig->access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "verify_token");
    if (s) {
        if (ig->verify_token)
            a->free(a->ctx, ig->verify_token, strlen(ig->verify_token) + 1);
        ig->verify_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "app_secret");
    if (s) {
        if (ig->app_secret)
            a->free(a->ctx, ig->app_secret, strlen(ig->app_secret) + 1);
        ig->app_secret = sc_strdup(a, s);
    }
}

static void parse_twitter_channel(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_twitter_channel_config_t *tw = &cfg->channels.twitter;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "api_key");
    if (s) {
        if (tw->api_key)
            a->free(a->ctx, tw->api_key, strlen(tw->api_key) + 1);
        tw->api_key = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "api_secret");
    if (s) {
        if (tw->api_secret)
            a->free(a->ctx, tw->api_secret, strlen(tw->api_secret) + 1);
        tw->api_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token");
    if (s) {
        if (tw->access_token)
            a->free(a->ctx, tw->access_token, strlen(tw->access_token) + 1);
        tw->access_token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "access_token_secret");
    if (s) {
        if (tw->access_token_secret)
            a->free(a->ctx, tw->access_token_secret, strlen(tw->access_token_secret) + 1);
        tw->access_token_secret = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "bearer_token");
    if (s) {
        if (tw->bearer_token)
            a->free(a->ctx, tw->bearer_token, strlen(tw->bearer_token) + 1);
        tw->bearer_token = sc_strdup(a, s);
    }
}

static void parse_google_rcs_channel(sc_allocator_t *a, sc_config_t *cfg,
                                     const sc_json_value_t *obj) {
    if (!obj)
        return;
    sc_google_rcs_channel_config_t *rcs = &cfg->channels.google_rcs;
    const sc_json_value_t *val = obj;
    if (obj->type == SC_JSON_ARRAY && obj->data.array.len > 0 && obj->data.array.items)
        val = obj->data.array.items[0];
    if (val->type != SC_JSON_OBJECT)
        return;
    const char *s = sc_json_get_string(val, "agent_id");
    if (s) {
        if (rcs->agent_id)
            a->free(a->ctx, rcs->agent_id, strlen(rcs->agent_id) + 1);
        rcs->agent_id = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "token");
    if (s) {
        if (rcs->token)
            a->free(a->ctx, rcs->token, strlen(rcs->token) + 1);
        rcs->token = sc_strdup(a, s);
    }
    s = sc_json_get_string(val, "service_account_json_path");
    if (s) {
        if (rcs->service_account_json_path)
            a->free(a->ctx, rcs->service_account_json_path,
                    strlen(rcs->service_account_json_path) + 1);
        rcs->service_account_json_path = sc_strdup(a, s);
    }
}

static sc_error_t parse_channels(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->channels.cli = sc_json_get_bool(obj, "cli", cfg->channels.cli);
    cfg->channels.suppress_tool_progress =
        sc_json_get_bool(obj, "suppress_tool_progress", cfg->channels.suppress_tool_progress);
    const char *def_ch = sc_json_get_string(obj, "default_channel");
    if (def_ch) {
        if (cfg->channels.default_channel)
            a->free(a->ctx, cfg->channels.default_channel,
                    strlen(cfg->channels.default_channel) + 1);
        cfg->channels.default_channel = sc_strdup(a, def_ch);
    }

    sc_json_value_t *email_obj = sc_json_object_get(obj, "email");
    if (email_obj)
        parse_email_channel(a, cfg, email_obj);

    sc_json_value_t *imap_obj = sc_json_object_get(obj, "imap");
    if (imap_obj)
        parse_imap_channel(a, cfg, imap_obj);

    sc_json_value_t *imsg_obj = sc_json_object_get(obj, "imessage");
    if (imsg_obj)
        parse_imessage_channel(a, cfg, imsg_obj);

    sc_json_value_t *gmail_obj = sc_json_object_get(obj, "gmail");
    if (gmail_obj)
        parse_gmail_channel(a, cfg, gmail_obj);

    sc_json_value_t *telegram_obj = sc_json_object_get(obj, "telegram");
    if (telegram_obj)
        parse_telegram_channel(a, cfg, telegram_obj);

    sc_json_value_t *discord_obj = sc_json_object_get(obj, "discord");
    if (discord_obj)
        parse_discord_channel(a, cfg, discord_obj);

    sc_json_value_t *slack_obj = sc_json_object_get(obj, "slack");
    if (slack_obj)
        parse_slack_channel(a, cfg, slack_obj);

    sc_json_value_t *whatsapp_obj = sc_json_object_get(obj, "whatsapp");
    if (whatsapp_obj)
        parse_whatsapp_channel(a, cfg, whatsapp_obj);

    sc_json_value_t *line_obj = sc_json_object_get(obj, "line");
    if (line_obj)
        parse_line_channel(a, cfg, line_obj);

    sc_json_value_t *google_chat_obj = sc_json_object_get(obj, "google_chat");
    if (google_chat_obj)
        parse_google_chat_channel(a, cfg, google_chat_obj);

    sc_json_value_t *facebook_obj = sc_json_object_get(obj, "facebook");
    if (facebook_obj)
        parse_facebook_channel(a, cfg, facebook_obj);

    sc_json_value_t *instagram_obj = sc_json_object_get(obj, "instagram");
    if (instagram_obj)
        parse_instagram_channel(a, cfg, instagram_obj);

    sc_json_value_t *twitter_obj = sc_json_object_get(obj, "twitter");
    if (twitter_obj)
        parse_twitter_channel(a, cfg, twitter_obj);

    sc_json_value_t *google_rcs_obj = sc_json_object_get(obj, "google_rcs");
    if (google_rcs_obj)
        parse_google_rcs_channel(a, cfg, google_rcs_obj);

    cfg->channels.channel_config_len = 0;
    if (obj->data.object.pairs && cfg->channels.channel_config_len < SC_CHANNEL_CONFIG_MAX) {
        for (size_t i = 0; i < obj->data.object.len; i++) {
            sc_json_pair_t *p = &obj->data.object.pairs[i];
            if (!p->key || !p->value)
                continue;
            if (strcmp(p->key, "cli") == 0 || strcmp(p->key, "default_channel") == 0)
                continue;
            size_t cnt = 0;
            if (p->value->type == SC_JSON_ARRAY && p->value->data.array.items)
                cnt = p->value->data.array.len;
            else if (p->value->type == SC_JSON_OBJECT && p->value->data.object.pairs)
                cnt = (p->value->data.object.len > 0) ? 1 : 0;
            else if (p->value->type == SC_JSON_OBJECT || p->value->type == SC_JSON_ARRAY)
                cnt = 1;
            if (cnt == 0)
                continue;
            if (cfg->channels.channel_config_len >= SC_CHANNEL_CONFIG_MAX)
                break;
            size_t klen = p->key_len > 0 ? p->key_len : strlen(p->key);
            char *k = (char *)a->alloc(a->ctx, klen + 1);
            if (!k)
                break;
            memcpy(k, p->key, klen);
            k[klen] = '\0';
            cfg->channels.channel_config_keys[cfg->channels.channel_config_len] = k;
            cfg->channels.channel_config_counts[cfg->channels.channel_config_len] = cnt;
            cfg->channels.channel_config_len++;
        }
    }
    return SC_OK;
}

static sc_error_t parse_mcp_servers(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->mcp_servers_len = 0;
    for (size_t i = 0; i < obj->data.object.len && cfg->mcp_servers_len < SC_MCP_SERVERS_MAX; i++) {
        sc_json_pair_t *p = &obj->data.object.pairs[i];
        if (!p->key || !p->value || p->value->type != SC_JSON_OBJECT)
            continue;

        sc_mcp_server_entry_t *entry = &cfg->mcp_servers[cfg->mcp_servers_len];
        memset(entry, 0, sizeof(*entry));
        entry->name = sc_strdup(a, p->key);

        const char *cmd = sc_json_get_string(p->value, "command");
        if (cmd)
            entry->command = sc_strdup(a, cmd);

        sc_json_value_t *args_arr = sc_json_object_get(p->value, "args");
        if (args_arr && args_arr->type == SC_JSON_ARRAY) {
            for (size_t j = 0;
                 j < args_arr->data.array.len && entry->args_count < SC_MCP_SERVER_ARGS_MAX; j++) {
                sc_json_value_t *arg_val = args_arr->data.array.items[j];
                if (arg_val && arg_val->type == SC_JSON_STRING && arg_val->data.string.ptr) {
                    entry->args[entry->args_count++] = sc_strdup(a, arg_val->data.string.ptr);
                }
            }
        }
        cfg->mcp_servers_len++;
    }
    return SC_OK;
}

static sc_error_t parse_agent(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->agent.compact_context =
        sc_json_get_bool(obj, "compact_context", cfg->agent.compact_context);
    double mti = sc_json_get_number(obj, "max_tool_iterations", cfg->agent.max_tool_iterations);
    if (mti >= 0 && mti <= 100000)
        cfg->agent.max_tool_iterations = (uint32_t)mti;
    double mhm = sc_json_get_number(obj, "max_history_messages", cfg->agent.max_history_messages);
    if (mhm >= 0 && mhm <= 10000)
        cfg->agent.max_history_messages = (uint32_t)mhm;
    cfg->agent.parallel_tools = sc_json_get_bool(obj, "parallel_tools", cfg->agent.parallel_tools);
    const char *td = sc_json_get_string(obj, "tool_dispatcher");
    if (td) {
        if (cfg->agent.tool_dispatcher)
            a->free(a->ctx, cfg->agent.tool_dispatcher, strlen(cfg->agent.tool_dispatcher) + 1);
        cfg->agent.tool_dispatcher = sc_strdup(a, td);
    }
    double tl = sc_json_get_number(obj, "token_limit", cfg->agent.token_limit);
    if (tl >= 0 && tl <= 2000000)
        cfg->agent.token_limit = (uint64_t)tl;
    double sids =
        sc_json_get_number(obj, "session_idle_timeout_secs", cfg->agent.session_idle_timeout_secs);
    if (sids >= 0)
        cfg->agent.session_idle_timeout_secs = (uint64_t)sids;
    double ckr =
        sc_json_get_number(obj, "compaction_keep_recent", cfg->agent.compaction_keep_recent);
    if (ckr >= 0 && ckr <= 200)
        cfg->agent.compaction_keep_recent = (uint32_t)ckr;
    double cms = sc_json_get_number(obj, "compaction_max_summary_chars",
                                    cfg->agent.compaction_max_summary_chars);
    if (cms >= 0 && cms <= 50000)
        cfg->agent.compaction_max_summary_chars = (uint32_t)cms;
    double cmx = sc_json_get_number(obj, "compaction_max_source_chars",
                                    cfg->agent.compaction_max_source_chars);
    if (cmx >= 0 && cmx <= 100000)
        cfg->agent.compaction_max_source_chars = (uint32_t)cmx;
    double mts = sc_json_get_number(obj, "message_timeout_secs", cfg->agent.message_timeout_secs);
    if (mts >= 0)
        cfg->agent.message_timeout_secs = (uint64_t)mts;
    double pmc = sc_json_get_number(obj, "pool_max_concurrent", cfg->agent.pool_max_concurrent);
    if (pmc >= 1 && pmc <= 64)
        cfg->agent.pool_max_concurrent = (uint32_t)pmc;
    const char *dp = sc_json_get_string(obj, "default_profile");
    if (dp) {
        if (cfg->agent.default_profile)
            a->free(a->ctx, cfg->agent.default_profile, strlen(cfg->agent.default_profile) + 1);
        cfg->agent.default_profile = sc_strdup(a, dp);
    }
    double cpw = sc_json_get_number(obj, "context_pressure_warn", cfg->agent.context_pressure_warn);
    if (cpw > 0.0 && cpw <= 1.0)
        cfg->agent.context_pressure_warn = (float)cpw;
    double cpc =
        sc_json_get_number(obj, "context_pressure_compact", cfg->agent.context_pressure_compact);
    if (cpc > 0.0 && cpc <= 1.0)
        cfg->agent.context_pressure_compact = (float)cpc;
    double cct =
        sc_json_get_number(obj, "context_compact_target", cfg->agent.context_compact_target);
    if (cct > 0.0 && cct <= 1.0)
        cfg->agent.context_compact_target = (float)cct;
    const char *persona = sc_json_get_string(obj, "persona");
    if (persona) {
        if (cfg->agent.persona)
            a->free(a->ctx, cfg->agent.persona, strlen(cfg->agent.persona) + 1);
        cfg->agent.persona = sc_strdup(a, persona);
    }
    return SC_OK;
}
static sc_error_t parse_policy_cfg(sc_allocator_t *a, sc_config_t *cfg,
                                   const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->policy.enabled = sc_json_get_bool(obj, "enabled", cfg->policy.enabled);
    const char *rj = sc_json_get_string(obj, "rules_json");
    if (rj) {
        if (cfg->policy.rules_json)
            a->free(a->ctx, cfg->policy.rules_json, strlen(cfg->policy.rules_json) + 1);
        cfg->policy.rules_json = sc_strdup(a, rj);
    }
    return SC_OK;
}
static sc_error_t parse_plugins_cfg(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj)
        return SC_OK;
    const sc_json_value_t *paths_arr = NULL;
    if (obj->type == SC_JSON_OBJECT) {
        cfg->plugins.enabled = sc_json_get_bool(obj, "enabled", cfg->plugins.enabled);
        paths_arr = sc_json_object_get(obj, "paths");
        const char *pd = sc_json_get_string(obj, "plugin_dir");
        if (pd) {
            if (cfg->plugins.plugin_dir)
                a->free(a->ctx, cfg->plugins.plugin_dir, strlen(cfg->plugins.plugin_dir) + 1);
            cfg->plugins.plugin_dir = sc_strdup(a, pd);
        }
    } else if (obj->type == SC_JSON_ARRAY) {
        paths_arr = obj;
    } else {
        return SC_OK;
    }
    if (!paths_arr || paths_arr->type != SC_JSON_ARRAY)
        return SC_OK;
    size_t n = paths_arr->data.array.len;
    if (n == 0)
        return SC_OK;
    if (cfg->plugins.plugin_paths) {
        for (size_t i = 0; i < cfg->plugins.plugin_paths_len; i++)
            if (cfg->plugins.plugin_paths[i])
                a->free(a->ctx, cfg->plugins.plugin_paths[i],
                        strlen(cfg->plugins.plugin_paths[i]) + 1);
        a->free(a->ctx, cfg->plugins.plugin_paths, cfg->plugins.plugin_paths_len * sizeof(char *));
        cfg->plugins.plugin_paths = NULL;
        cfg->plugins.plugin_paths_len = 0;
    }
    cfg->plugins.plugin_paths = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!cfg->plugins.plugin_paths)
        return SC_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < n; i++) {
        const sc_json_value_t *item = paths_arr->data.array.items[i];
        if (item && item->type == SC_JSON_STRING && item->data.string.ptr)
            cfg->plugins.plugin_paths[i] =
                sc_strndup(a, item->data.string.ptr, item->data.string.len);
        else
            cfg->plugins.plugin_paths[i] = NULL;
    }
    cfg->plugins.plugin_paths_len = n;
    return SC_OK;
}
static sc_error_t parse_heartbeat(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->heartbeat.enabled = sc_json_get_bool(obj, "enabled", cfg->heartbeat.enabled);
    double im = sc_json_get_number(obj, "interval_minutes", cfg->heartbeat.interval_minutes);
    if (!im)
        im = sc_json_get_number(obj, "interval_secs", 0);
    if (im > 0 && im <= 1440)
        cfg->heartbeat.interval_minutes = (uint32_t)im;
    return SC_OK;
}

static sc_error_t parse_reliability(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    const char *pp = sc_json_get_string(obj, "primary_provider");
    if (pp) {
        if (cfg->reliability.primary_provider)
            a->free(a->ctx, cfg->reliability.primary_provider,
                    strlen(cfg->reliability.primary_provider) + 1);
        cfg->reliability.primary_provider = sc_strdup(a, pp);
    }
    double pr = sc_json_get_number(obj, "provider_retries", cfg->reliability.provider_retries);
    if (pr >= 0 && pr <= 20)
        cfg->reliability.provider_retries = (uint32_t)pr;
    double pbm =
        sc_json_get_number(obj, "provider_backoff_ms", cfg->reliability.provider_backoff_ms);
    if (pbm >= 0)
        cfg->reliability.provider_backoff_ms = (uint64_t)pbm;
    double cibs = sc_json_get_number(obj, "channel_initial_backoff_secs",
                                     cfg->reliability.channel_initial_backoff_secs);
    if (cibs >= 0)
        cfg->reliability.channel_initial_backoff_secs = (uint64_t)cibs;
    double cmbs = sc_json_get_number(obj, "channel_max_backoff_secs",
                                     cfg->reliability.channel_max_backoff_secs);
    if (cmbs >= 0)
        cfg->reliability.channel_max_backoff_secs = (uint64_t)cmbs;
    double sps =
        sc_json_get_number(obj, "scheduler_poll_secs", cfg->reliability.scheduler_poll_secs);
    if (sps >= 0)
        cfg->reliability.scheduler_poll_secs = (uint64_t)sps;
    double sr = sc_json_get_number(obj, "scheduler_retries", cfg->reliability.scheduler_retries);
    if (sr >= 0 && sr <= 20)
        cfg->reliability.scheduler_retries = (uint32_t)sr;
    sc_json_value_t *fp = sc_json_object_get(obj, "fallback_providers");
    if (fp && fp->type == SC_JSON_ARRAY) {
        if (cfg->reliability.fallback_providers) {
            for (size_t i = 0; i < cfg->reliability.fallback_providers_len; i++)
                a->free(a->ctx, cfg->reliability.fallback_providers[i],
                        strlen(cfg->reliability.fallback_providers[i]) + 1);
            a->free(a->ctx, cfg->reliability.fallback_providers,
                    cfg->reliability.fallback_providers_len * sizeof(char *));
        }
        parse_string_array(a, &cfg->reliability.fallback_providers,
                           &cfg->reliability.fallback_providers_len, fp);
    }
    return SC_OK;
}

static sc_error_t parse_router(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    const char *fast = sc_json_get_string(obj, "fast");
    if (fast) {
        if (cfg->router.fast)
            a->free(a->ctx, cfg->router.fast, strlen(cfg->router.fast) + 1);
        cfg->router.fast = sc_strdup(a, fast);
    }
    const char *standard = sc_json_get_string(obj, "standard");
    if (standard) {
        if (cfg->router.standard)
            a->free(a->ctx, cfg->router.standard, strlen(cfg->router.standard) + 1);
        cfg->router.standard = sc_strdup(a, standard);
    }
    const char *powerful = sc_json_get_string(obj, "powerful");
    if (powerful) {
        if (cfg->router.powerful)
            a->free(a->ctx, cfg->router.powerful, strlen(cfg->router.powerful) + 1);
        cfg->router.powerful = sc_strdup(a, powerful);
    }
    double cl = sc_json_get_number(obj, "complexity_low", (double)cfg->router.complexity_low);
    if (cl >= 0 && cl <= 10000)
        cfg->router.complexity_low = (int)cl;
    double ch = sc_json_get_number(obj, "complexity_high", (double)cfg->router.complexity_high);
    if (ch >= 0 && ch <= 100000)
        cfg->router.complexity_high = (int)ch;
    return SC_OK;
}

static sc_error_t parse_session(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    double im = sc_json_get_number(obj, "idle_minutes", cfg->session.idle_minutes);
    if (im >= 0 && im <= 1440)
        cfg->session.idle_minutes = (uint32_t)im;
    const char *scope = sc_json_get_string(obj, "dm_scope");
    if (scope) {
        if (strcmp(scope, "main") == 0)
            cfg->session.dm_scope = DirectScopeMain;
        else if (strcmp(scope, "per_peer") == 0)
            cfg->session.dm_scope = DirectScopePerPeer;
        else if (strcmp(scope, "per_channel_peer") == 0)
            cfg->session.dm_scope = DirectScopePerChannelPeer;
        else if (strcmp(scope, "per_account_channel_peer") == 0)
            cfg->session.dm_scope = DirectScopePerAccountChannelPeer;
    }
    return SC_OK;
}

static sc_error_t parse_peripherals(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->peripherals.enabled = sc_json_get_bool(obj, "enabled", cfg->peripherals.enabled);
    const char *dd = sc_json_get_string(obj, "datasheet_dir");
    if (dd) {
        if (cfg->peripherals.datasheet_dir)
            a->free(a->ctx, cfg->peripherals.datasheet_dir,
                    strlen(cfg->peripherals.datasheet_dir) + 1);
        cfg->peripherals.datasheet_dir = sc_strdup(a, dd);
    }
    return SC_OK;
}

static sc_error_t parse_hardware(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->hardware.enabled = sc_json_get_bool(obj, "enabled", cfg->hardware.enabled);
    const char *transport = sc_json_get_string(obj, "transport");
    if (transport) {
        if (cfg->hardware.transport)
            a->free(a->ctx, cfg->hardware.transport, strlen(cfg->hardware.transport) + 1);
        cfg->hardware.transport = sc_strdup(a, transport);
    }
    const char *serial_port = sc_json_get_string(obj, "serial_port");
    if (serial_port) {
        if (cfg->hardware.serial_port)
            a->free(a->ctx, cfg->hardware.serial_port, strlen(cfg->hardware.serial_port) + 1);
        cfg->hardware.serial_port = sc_strdup(a, serial_port);
    }
    double br = sc_json_get_number(obj, "baud_rate", cfg->hardware.baud_rate);
    if (br >= 0 && br <= 4000000)
        cfg->hardware.baud_rate = (uint32_t)br;
    const char *probe_target = sc_json_get_string(obj, "probe_target");
    if (probe_target) {
        if (cfg->hardware.probe_target)
            a->free(a->ctx, cfg->hardware.probe_target, strlen(cfg->hardware.probe_target) + 1);
        cfg->hardware.probe_target = sc_strdup(a, probe_target);
    }
    return SC_OK;
}

static sc_error_t parse_browser(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->browser.enabled = sc_json_get_bool(obj, "enabled", cfg->browser.enabled);
    return SC_OK;
}

static sc_error_t parse_cost(sc_allocator_t *a, sc_config_t *cfg, const sc_json_value_t *obj) {
    (void)a;
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    cfg->cost.enabled = sc_json_get_bool(obj, "enabled", cfg->cost.enabled);
    double dl = sc_json_get_number(obj, "daily_limit_usd", cfg->cost.daily_limit_usd);
    if (dl >= 0 && dl <= 1000000.0)
        cfg->cost.daily_limit_usd = dl;
    double ml = sc_json_get_number(obj, "monthly_limit_usd", cfg->cost.monthly_limit_usd);
    if (ml >= 0 && ml <= 10000000.0)
        cfg->cost.monthly_limit_usd = ml;
    double wp = sc_json_get_number(obj, "warn_at_percent", cfg->cost.warn_at_percent);
    if (wp >= 0 && wp <= 100)
        cfg->cost.warn_at_percent = (uint8_t)wp;
    cfg->cost.allow_override = sc_json_get_bool(obj, "allow_override", cfg->cost.allow_override);
    return SC_OK;
}

static sc_error_t parse_diagnostics(sc_allocator_t *a, sc_config_t *cfg,
                                    const sc_json_value_t *obj) {
    if (!obj || obj->type != SC_JSON_OBJECT)
        return SC_OK;
    const char *backend = sc_json_get_string(obj, "backend");
    if (backend) {
        if (cfg->diagnostics.backend)
            a->free(a->ctx, cfg->diagnostics.backend, strlen(cfg->diagnostics.backend) + 1);
        cfg->diagnostics.backend = sc_strdup(a, backend);
    }
    const char *ote = sc_json_get_string(obj, "otel_endpoint");
    if (ote) {
        if (cfg->diagnostics.otel_endpoint)
            a->free(a->ctx, cfg->diagnostics.otel_endpoint,
                    strlen(cfg->diagnostics.otel_endpoint) + 1);
        cfg->diagnostics.otel_endpoint = sc_strdup(a, ote);
    }
    const char *ots = sc_json_get_string(obj, "otel_service_name");
    if (ots) {
        if (cfg->diagnostics.otel_service_name)
            a->free(a->ctx, cfg->diagnostics.otel_service_name,
                    strlen(cfg->diagnostics.otel_service_name) + 1);
        cfg->diagnostics.otel_service_name = sc_strdup(a, ots);
    }
    cfg->diagnostics.log_tool_calls =
        sc_json_get_bool(obj, "log_tool_calls", cfg->diagnostics.log_tool_calls);
    cfg->diagnostics.log_message_receipts =
        sc_json_get_bool(obj, "log_message_receipts", cfg->diagnostics.log_message_receipts);
    cfg->diagnostics.log_message_payloads =
        sc_json_get_bool(obj, "log_message_payloads", cfg->diagnostics.log_message_payloads);
    cfg->diagnostics.log_llm_io = sc_json_get_bool(obj, "log_llm_io", cfg->diagnostics.log_llm_io);
    if (cfg->diagnostics.log_llm_io || cfg->diagnostics.log_message_payloads) {
        fprintf(stderr, "[SECURITY WARNING] Diagnostic logging of payloads is enabled. "
                        "Logs may contain sensitive data (PII, API keys). "
                        "Do not use in production.\n");
    }
    return SC_OK;
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
                        sc_json_parse(&a, vbuf, vread, &validation_root);
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

void sc_config_deinit(sc_config_t *cfg) {
    if (!cfg)
        return;
    if (cfg->arena) {
        sc_arena_destroy(cfg->arena);
        cfg->arena = NULL;
    }
    memset(cfg, 0, sizeof(*cfg));
}

sc_error_t sc_config_parse_json(sc_config_t *cfg, const char *content, size_t len) {
    if (!cfg || !content)
        return SC_ERR_INVALID_ARGUMENT;
    sc_allocator_t *a = &cfg->allocator;

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(a, content, len, &root);
    if (err != SC_OK)
        return err;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(a, root);
        return SC_ERR_JSON_PARSE;
    }

    sc_error_t mig_err = sc_config_migrate(a, root);
    if (mig_err != SC_OK) {
        sc_json_free(a, root);
        return mig_err;
    }

    cfg->config_version = (int)sc_json_get_number(root, "config_version", 1.0);

    const char *workspace = sc_json_get_string(root, "workspace");
    if (workspace && strstr(workspace, "..")) {
        workspace = NULL; /* Reject path traversal */
    }
    if (workspace && cfg->workspace_dir_override) {
        a->free(a->ctx, cfg->workspace_dir_override, strlen(cfg->workspace_dir_override) + 1);
    }
    if (workspace) {
        cfg->workspace_dir_override = sc_strdup(a, workspace);
        if (cfg->workspace_dir)
            a->free(a->ctx, cfg->workspace_dir, strlen(cfg->workspace_dir) + 1);
        cfg->workspace_dir = sc_strdup(a, workspace);
    }

    const char *prov = sc_json_get_string(root, "default_provider");
    if (prov) {
        if (cfg->default_provider)
            a->free(a->ctx, cfg->default_provider, strlen(cfg->default_provider) + 1);
        cfg->default_provider = sc_strdup(a, prov);
    }
    const char *model = sc_json_get_string(root, "default_model");
    if (model && model[0]) {
        if (cfg->default_model)
            a->free(a->ctx, cfg->default_model, strlen(cfg->default_model) + 1);
        cfg->default_model = sc_strdup(a, model);
    }
    double temp = sc_json_get_number(root, "default_temperature", cfg->default_temperature);
    if (temp >= 0.0 && temp <= 2.0)
        cfg->default_temperature = temp;

    double mt = sc_json_get_number(root, "max_tokens", (double)cfg->max_tokens);
    if (mt >= 0.0 && mt <= 1000000)
        cfg->max_tokens = (uint32_t)mt;

    sc_json_value_t *prov_arr = sc_json_object_get(root, "providers");
    if (prov_arr) {
        if (prov_arr->type == SC_JSON_ARRAY)
            parse_providers(a, cfg, prov_arr);
        else if (prov_arr->type == SC_JSON_OBJECT) {
            sc_json_value_t *router_obj = sc_json_object_get(prov_arr, "router");
            if (router_obj)
                parse_router(a, cfg, router_obj);
        }
    }

    sc_json_value_t *router_obj = sc_json_object_get(root, "router");
    if (router_obj)
        parse_router(a, cfg, router_obj);

    sc_json_value_t *aut = sc_json_object_get(root, "autonomy");
    if (aut)
        parse_autonomy(a, cfg, aut);

    sc_json_value_t *gw = sc_json_object_get(root, "gateway");
    if (gw)
        parse_gateway(a, cfg, gw);

    sc_json_value_t *mem = sc_json_object_get(root, "memory");
    if (mem)
        parse_memory(a, cfg, mem);

    sc_json_value_t *tools_obj = sc_json_object_get(root, "tools");
    if (tools_obj)
        parse_tools(a, cfg, tools_obj);

    sc_json_value_t *cron_obj = sc_json_object_get(root, "cron");
    if (cron_obj)
        parse_cron(a, cfg, cron_obj);

    sc_json_value_t *sched_obj = sc_json_object_get(root, "scheduler");
    if (sched_obj)
        parse_scheduler(a, cfg, sched_obj);

    sc_json_value_t *rt_obj = sc_json_object_get(root, "runtime");
    if (rt_obj)
        parse_runtime(a, cfg, rt_obj);

    sc_json_value_t *tunnel_obj = sc_json_object_get(root, "tunnel");
    if (tunnel_obj)
        parse_tunnel(a, cfg, tunnel_obj);

    sc_json_value_t *ch_obj = sc_json_object_get(root, "channels");
    if (ch_obj)
        parse_channels(a, cfg, ch_obj);

    sc_json_value_t *agent_obj = sc_json_object_get(root, "agent");
    if (agent_obj)
        parse_agent(a, cfg, agent_obj);

    sc_json_value_t *heartbeat_obj = sc_json_object_get(root, "heartbeat");
    if (heartbeat_obj)
        parse_heartbeat(a, cfg, heartbeat_obj);

    sc_json_value_t *reliability_obj = sc_json_object_get(root, "reliability");
    if (reliability_obj)
        parse_reliability(a, cfg, reliability_obj);

    sc_json_value_t *diagnostics_obj = sc_json_object_get(root, "diagnostics");
    if (diagnostics_obj)
        parse_diagnostics(a, cfg, diagnostics_obj);

    sc_json_value_t *session_obj = sc_json_object_get(root, "session");
    if (session_obj)
        parse_session(a, cfg, session_obj);

    sc_json_value_t *peripherals_obj = sc_json_object_get(root, "peripherals");
    if (peripherals_obj)
        parse_peripherals(a, cfg, peripherals_obj);

    sc_json_value_t *hardware_obj = sc_json_object_get(root, "hardware");
    if (hardware_obj)
        parse_hardware(a, cfg, hardware_obj);

    sc_json_value_t *browser_obj = sc_json_object_get(root, "browser");
    if (browser_obj)
        parse_browser(a, cfg, browser_obj);

    sc_json_value_t *cost_obj = sc_json_object_get(root, "cost");
    if (cost_obj)
        parse_cost(a, cfg, cost_obj);

    sc_json_value_t *mcp_obj = sc_json_object_get(root, "mcp_servers");
    if (mcp_obj)
        parse_mcp_servers(a, cfg, mcp_obj);

    sc_json_value_t *nodes_obj = sc_json_object_get(root, "nodes");
    if (nodes_obj)
        parse_nodes(a, cfg, nodes_obj);

    sc_json_value_t *policy_obj = sc_json_object_get(root, "policy");
    if (policy_obj)
        parse_policy_cfg(a, cfg, policy_obj);
    sc_json_value_t *plugins_obj = sc_json_object_get(root, "plugins");
    if (plugins_obj)
        parse_plugins_cfg(a, cfg, plugins_obj);
    sc_json_value_t *sec = sc_json_object_get(root, "security");
    if (sec && sec->type == SC_JSON_OBJECT) {
        double al = sc_json_get_number(sec, "autonomy_level", cfg->security.autonomy_level);
        if (al < 0)
            al = 0;
        else if (al > 4)
            al = 4;
        cfg->security.autonomy_level = (uint8_t)al;
        const char *sb = sc_json_get_string(sec, "sandbox");
        if (sb) {
            if (cfg->security.sandbox)
                a->free(a->ctx, cfg->security.sandbox, strlen(cfg->security.sandbox) + 1);
            cfg->security.sandbox = sc_strdup(a, sb);
            cfg->security.sandbox_config.backend = parse_sandbox_backend(sb);
        }
        sc_json_value_t *sbox = sc_json_object_get(sec, "sandbox_config");
        if (sbox && sbox->type == SC_JSON_OBJECT) {
            cfg->security.sandbox_config.enabled =
                sc_json_get_bool(sbox, "enabled", cfg->security.sandbox_config.enabled);
            const char *be = sc_json_get_string(sbox, "backend");
            if (be)
                cfg->security.sandbox_config.backend = parse_sandbox_backend(be);
            sc_json_value_t *fa = sc_json_object_get(sbox, "firejail_args");
            if (fa && fa->type == SC_JSON_ARRAY) {
                if (cfg->security.sandbox_config.firejail_args) {
                    for (size_t i = 0; i < cfg->security.sandbox_config.firejail_args_len; i++)
                        a->free(a->ctx, cfg->security.sandbox_config.firejail_args[i],
                                strlen(cfg->security.sandbox_config.firejail_args[i]) + 1);
                    a->free(a->ctx, cfg->security.sandbox_config.firejail_args,
                            cfg->security.sandbox_config.firejail_args_len * sizeof(char *));
                }
                parse_string_array(a, &cfg->security.sandbox_config.firejail_args,
                                   &cfg->security.sandbox_config.firejail_args_len, fa);
            }
            sc_json_value_t *np = sc_json_object_get(sbox, "net_proxy");
            if (np && np->type == SC_JSON_OBJECT) {
                cfg->security.sandbox_config.net_proxy.enabled =
                    sc_json_get_bool(np, "enabled", false);
                cfg->security.sandbox_config.net_proxy.deny_all =
                    sc_json_get_bool(np, "deny_all", true);
                const char *pa = sc_json_get_string(np, "proxy_addr");
                if (pa) {
                    if (cfg->security.sandbox_config.net_proxy.proxy_addr)
                        a->free(a->ctx, cfg->security.sandbox_config.net_proxy.proxy_addr,
                                strlen(cfg->security.sandbox_config.net_proxy.proxy_addr) + 1);
                    cfg->security.sandbox_config.net_proxy.proxy_addr = sc_strdup(a, pa);
                }
                sc_json_value_t *ad = sc_json_object_get(np, "allowed_domains");
                if (ad && ad->type == SC_JSON_ARRAY) {
                    if (cfg->security.sandbox_config.net_proxy.allowed_domains) {
                        for (size_t i = 0;
                             i < cfg->security.sandbox_config.net_proxy.allowed_domains_len; i++)
                            a->free(
                                a->ctx, cfg->security.sandbox_config.net_proxy.allowed_domains[i],
                                strlen(cfg->security.sandbox_config.net_proxy.allowed_domains[i]) +
                                    1);
                        a->free(a->ctx, cfg->security.sandbox_config.net_proxy.allowed_domains,
                                cfg->security.sandbox_config.net_proxy.allowed_domains_len *
                                    sizeof(char *));
                    }
                    parse_string_array(a, &cfg->security.sandbox_config.net_proxy.allowed_domains,
                                       &cfg->security.sandbox_config.net_proxy.allowed_domains_len,
                                       ad);
                }
            }
        }
        sc_json_value_t *res = sc_json_object_get(sec, "resources");
        if (res && res->type == SC_JSON_OBJECT) {
            double mfs = sc_json_get_number(res, "max_file_size",
                                            cfg->security.resource_limits.max_file_size);
            if (mfs >= 0)
                cfg->security.resource_limits.max_file_size = (uint64_t)mfs;
            double mrs = sc_json_get_number(res, "max_read_size",
                                            cfg->security.resource_limits.max_read_size);
            if (mrs >= 0)
                cfg->security.resource_limits.max_read_size = (uint64_t)mrs;
            double mmb = sc_json_get_number(res, "max_memory_mb",
                                            cfg->security.resource_limits.max_memory_mb);
            if (mmb >= 0 && mmb <= 1048576)
                cfg->security.resource_limits.max_memory_mb = (uint32_t)mmb;
        }
        sc_json_value_t *aud = sc_json_object_get(sec, "audit");
        if (aud && aud->type == SC_JSON_OBJECT) {
            cfg->security.audit.enabled =
                sc_json_get_bool(aud, "enabled", cfg->security.audit.enabled);
            const char *lp = sc_json_get_string(aud, "log_path");
            if (!lp)
                lp = sc_json_get_string(aud, "log_file");
            if (lp) {
                if (cfg->security.audit.log_path)
                    a->free(a->ctx, cfg->security.audit.log_path,
                            strlen(cfg->security.audit.log_path) + 1);
                cfg->security.audit.log_path = sc_strdup(a, lp);
            }
        }
    }

    sc_json_value_t *gw_obj = sc_json_object_get(root, "gateway");
    if (gw_obj) {
        double port = sc_json_get_number(gw_obj, "port", cfg->gateway.port);
        if (port < 1)
            port = 1;
        else if (port > 65535)
            port = 65535;
        cfg->gateway.port = (uint16_t)port;
    }

    const char *api_k_root = sc_json_get_string(root, "api_key");
    if (api_k_root && api_k_root[0]) {
        if (cfg->api_key)
            a->free(a->ctx, cfg->api_key, strlen(cfg->api_key) + 1);
        cfg->api_key = sc_strdup(a, api_k_root);
    }

    sc_json_value_t *secrets_obj = sc_json_object_get(root, "secrets");
    if (secrets_obj && secrets_obj->type == SC_JSON_OBJECT) {
        cfg->secrets.encrypt = sc_json_get_bool(secrets_obj, "encrypt", cfg->secrets.encrypt);
    }

    sc_json_value_t *identity_obj = sc_json_object_get(root, "identity");
    if (identity_obj && identity_obj->type == SC_JSON_OBJECT) {
        const char *fmt = sc_json_get_string(identity_obj, "format");
        if (fmt) {
            if (cfg->identity.format)
                a->free(a->ctx, cfg->identity.format, strlen(cfg->identity.format) + 1);
            cfg->identity.format = sc_strdup(a, fmt);
        }
    }

    sc_json_free(a, root);
    return SC_OK;
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

/* sc_config_save moved to config_serialize.c */
/* sc_config_get_*, sc_config_validate, sc_config_provider_requires_api_key moved to
 * config_getters.c */
