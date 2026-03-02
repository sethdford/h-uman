#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#include "seaclaw/core/error.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/config.h"
#include "seaclaw/agent.h"
#include "seaclaw/agent/cli.h"
#include "seaclaw/provider.h"
#include "seaclaw/channel.h"
#include "seaclaw/tool.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/security.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/health.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/runtime.h"
#include "seaclaw/onboard.h"
#include "seaclaw/daemon.h"
#include "seaclaw/skillforge.h"
#include "seaclaw/skill_registry.h"
#include "seaclaw/migration.h"
#include "seaclaw/crontab.h"
#include "seaclaw/cli_commands.h"
#include "seaclaw/doctor.h"
#include "seaclaw/gateway.h"
#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/mcp_server.h"
#include "seaclaw/session.h"
#include "seaclaw/cron.h"
#include "seaclaw/cost.h"
#include "seaclaw/bus.h"
#if SC_HAS_EMAIL
#include "seaclaw/channels/email.h"
#endif
#if SC_HAS_IMESSAGE
#include "seaclaw/channels/imessage.h"
#endif

#define SC_VERSION "0.1.0"
#define SC_CODENAME "seaclaw"

typedef struct sc_command {
    const char *name;
    const char *description;
    sc_error_t (*handler)(sc_allocator_t *alloc, int argc, char **argv);
} sc_command_t;

static sc_error_t cmd_agent(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_gateway(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_mcp(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_version(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_help(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_status(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_doctor(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_cron(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_onboard(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_service(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_service_loop(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_migrate(sc_allocator_t *alloc, int argc, char **argv);

static const sc_command_t commands[] = {
    {"agent", "Start interactive agent session", cmd_agent},
    {"gateway", "Start webhook gateway server", cmd_gateway},
    {"mcp", "Run as MCP server (stdin/stdout JSON-RPC)", cmd_mcp},
    {"service", "Run as background service (daemonize)", cmd_service},
    {"service-loop", "Run service loop in foreground", cmd_service_loop},
    {"status", "Show runtime status", cmd_status},
    {"onboard", "Interactive setup wizard", cmd_onboard},
    {"doctor", "Run system diagnostics", cmd_doctor},
    {"cron", "Manage scheduled tasks", cmd_cron},
    {"channel", "Channel management", cmd_channel},
    {"skills", "Skill discovery and integration", cmd_skills},
    {"hardware", "Hardware peripheral management", cmd_hardware},
    {"sandbox", "Show sandbox status and backends", cmd_sandbox},
    {"migrate", "Migrate memory backends", cmd_migrate},
    {"memory", "Memory operations", cmd_memory},
    {"workspace", "Workspace management", cmd_workspace},
    {"capabilities", "Show available capabilities", cmd_capabilities},
    {"models", "List available models", cmd_models},
    {"auth", "Authentication management", cmd_auth},
    {"update", "Check for updates", cmd_update},
    {"version", "Show version information", cmd_version},
    {"help", "Show help information", cmd_help},
};

#define COMMANDS_COUNT (sizeof(commands) / sizeof(commands[0]))

static sc_command_t const *find_command(const char *name) {
    for (size_t i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(commands[i].name, name) == 0)
            return &commands[i];
    }
    return NULL;
}

static void print_usage(FILE *out) {
    fprintf(out, "%s v%s — Autonomous AI Assistant Runtime (C/ASM/WASM)\n\n", SC_CODENAME, SC_VERSION);
    fprintf(out, "Usage: seaclaw [command] [options]\n\n");
    fprintf(out, "Commands:\n");
    for (size_t i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(commands[i].name, "version") != 0 && strcmp(commands[i].name, "help") != 0)
            fprintf(out, "  %-14s %s\n", commands[i].name, commands[i].description);
    }
    fprintf(out, "  %-14s %s\n", "version", "Show version information");
    fprintf(out, "  %-14s %s\n", "help", "Show help information");
}

static sc_error_t cmd_version(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    printf("%s v%s\n", SC_CODENAME, SC_VERSION);
    return SC_OK;
}

static sc_error_t cmd_help(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    print_usage(stdout);
    return SC_OK;
}

static sc_error_t cmd_status(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    sc_readiness_result_t r = sc_health_check_readiness(alloc);
    printf("Status: %s\n", r.status == SC_READINESS_READY ? "ready" : "not ready");
    if (r.check_count > 0 && r.checks) {
        for (size_t i = 0; i < r.check_count; i++) {
            printf("  %s: %s\n", r.checks[i].name,
                r.checks[i].healthy ? "ok" : "error");
            if (r.checks[i].message)
                printf("    %s\n", r.checks[i].message);
        }
        alloc->free(alloc->ctx, (void *)r.checks,
            r.check_count * sizeof(sc_component_check_t));
    } else if (r.check_count == 0) {
        printf("  (no components registered)\n");
    }
    return SC_OK;
}

static sc_error_t cmd_doctor(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        printf("[doctor] config: error — %s\n", sc_error_string(err));
        return err;
    }

    printf("[doctor] config: ok (loaded from %s)\n",
        cfg.config_path && cfg.config_path[0] ? cfg.config_path : "defaults");

    const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
    if (sc_config_provider_requires_api_key(prov)) {
        const char *key = sc_config_default_provider_key(&cfg);
        if (key && key[0]) {
            printf("[doctor] provider (%s): ok (API key configured)\n", prov);
        } else {
            printf("[doctor] provider (%s): warning — no API key\n", prov);
        }
    } else {
        printf("[doctor] provider (%s): ok (local — no API key required)\n", prov);
    }

    const char *backend = cfg.memory_backend ? cfg.memory_backend : "none";
    printf("[doctor] memory engine: %s\n", backend);

    sc_diag_item_t *items = NULL;
    size_t item_count = 0;
    err = sc_doctor_check_config_semantics(alloc, &cfg, &items, &item_count);
    if (err == SC_OK && items && item_count > 0) {
        for (size_t i = 0; i < item_count; i++) {
            const char *sev = items[i].severity == SC_DIAG_ERR ? "error" :
                              items[i].severity == SC_DIAG_WARN ? "warning" : "info";
            printf("[doctor] %s: %s — %s\n", sev,
                items[i].category ? items[i].category : "config",
                items[i].message ? items[i].message : "");
        }
    }
    if (items) {
        for (size_t i = 0; i < item_count; i++) {
            if (items[i].category) alloc->free(alloc->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
            if (items[i].message) alloc->free(alloc->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
        }
        alloc->free(alloc->ctx, items, item_count * sizeof(sc_diag_item_t));
    }

    sc_config_deinit(&cfg);
    return SC_OK;
}

static sc_error_t cmd_cron(sc_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "";

    char *path = NULL;
    size_t path_len = 0;
    sc_error_t err = sc_crontab_get_path(alloc, &path, &path_len);
    if (err != SC_OK || !path) {
        fprintf(stderr, "[%s] cron: failed to get crontab path\n", SC_CODENAME);
        return err;
    }
    path[path_len] = '\0';

    if (strcmp(sub, "list") == 0) {
        sc_crontab_entry_t *entries = NULL;
        size_t count = 0;
        err = sc_crontab_load(alloc, path, &entries, &count);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] cron list: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        if (count == 0) {
            printf("No scheduled tasks.\n");
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("%s  %s  %s  %s\n",
                    entries[i].id,
                    entries[i].schedule,
                    entries[i].command,
                    entries[i].enabled ? "enabled" : "disabled");
            }
        }
        sc_crontab_entries_free(alloc, entries, count);
        return SC_OK;
    }

    if (strcmp(sub, "add") == 0) {
        if (argc < 5 || !argv[3] || !argv[4]) {
            alloc->free(alloc->ctx, path, path_len + 1);
            fprintf(stderr, "[%s] cron add <schedule> <command>\n", SC_CODENAME);
            return SC_ERR_INVALID_ARGUMENT;
        }
        const char *schedule = argv[3];
        size_t schedule_len = strlen(schedule);
        size_t cmd_len = 0;
        for (int i = 4; i < argc && argv[i]; i++) {
            if (i > 4) cmd_len++;
            cmd_len += strlen(argv[i]);
        }
        char *command = (char *)alloc->alloc(alloc->ctx, cmd_len + 1);
        if (!command) {
            alloc->free(alloc->ctx, path, path_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t pos = 0;
        for (int i = 4; i < argc && argv[i]; i++) {
            if (i > 4) command[pos++] = ' ';
            size_t l = strlen(argv[i]);
            memcpy(command + pos, argv[i], l + 1);
            pos += l;
        }
        command[pos] = '\0';

        char *new_id = NULL;
        err = sc_crontab_add(alloc, path, schedule, schedule_len, command, pos, &new_id);
        alloc->free(alloc->ctx, command, cmd_len + 1);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] cron add: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        printf("Added cron job %s\n", new_id ? new_id : "");
        if (new_id) alloc->free(alloc->ctx, new_id, strlen(new_id) + 1);
        return SC_OK;
    }

    if (strcmp(sub, "remove") == 0) {
        if (argc < 4 || !argv[3]) {
            alloc->free(alloc->ctx, path, path_len + 1);
            fprintf(stderr, "[%s] cron remove <id>\n", SC_CODENAME);
            return SC_ERR_INVALID_ARGUMENT;
        }
        err = sc_crontab_remove(alloc, path, argv[3]);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] cron remove: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        printf("Removed cron job %s\n", argv[3]);
        return SC_OK;
    }

    alloc->free(alloc->ctx, path, path_len + 1);
    fprintf(stderr, "[%s] cron: use 'list', 'add', or 'remove'\n", SC_CODENAME);
    fprintf(stderr, "  seaclaw cron list\n");
    fprintf(stderr, "  seaclaw cron add <schedule> <command>\n");
    fprintf(stderr, "  seaclaw cron remove <id>\n");
    return SC_ERR_INVALID_ARGUMENT;
}

static sc_error_t cmd_onboard(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return sc_onboard_run(alloc);
}

static sc_error_t cmd_service(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    sc_error_t err = sc_daemon_start();
    if (err == SC_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "[%s] daemon not supported on this platform\n", SC_CODENAME);
        return err;
    }
    (void)alloc;
    return err;
}

static sc_memory_t service_create_memory(sc_allocator_t *alloc,
    const sc_config_t *cfg, const char *ws)
{
    const char *backend = cfg->memory.backend;
    if (!backend) backend = "markdown";

    if (strcmp(backend, "sqlite") == 0) {
        const char *path = cfg->memory.sqlite_path;
        char buf[1024];
        if (!path) {
            const char *home = getenv("HOME");
            if (home) {
                int n = snprintf(buf, sizeof(buf), "%s/.seaclaw/memory.db", home);
                if (n > 0 && (size_t)n < sizeof(buf)) path = buf;
            }
        }
        if (path) return sc_sqlite_memory_create(alloc, path);
        return sc_markdown_memory_create(alloc, ws);
    }
    if (strcmp(backend, "none") == 0) return sc_none_memory_create(alloc);

    if (strcmp(backend, "lru") == 0 || strcmp(backend, "memory") == 0)
        return sc_memory_lru_create(alloc, 256);

    if (strcmp(backend, "lancedb") == 0) {
        char buf2[1024];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.seaclaw/lancedb", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return sc_lancedb_memory_create(alloc, buf2);
        }
        return sc_lancedb_memory_create(alloc, ".seaclaw/lancedb");
    }

    if (strcmp(backend, "lucid") == 0) {
        char buf2[1024];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.seaclaw/lucid.db", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return sc_lucid_memory_create(alloc, buf2, ws);
        }
        return sc_lucid_memory_create(alloc, ".seaclaw/lucid.db", ws);
    }

#ifdef SC_ENABLE_POSTGRES
    if (strcmp(backend, "postgres") == 0) {
        const char *url = cfg->memory.postgres_url;
        if (!url) url = "postgres://localhost/seaclaw";
        const char *schema = cfg->memory.postgres_schema;
        if (!schema) schema = "public";
        const char *table = cfg->memory.postgres_table;
        if (!table) table = "memories";
        return sc_postgres_memory_create(alloc, url, schema, table);
    }
#endif

#ifdef SC_ENABLE_REDIS_ENGINE
    if (strcmp(backend, "redis") == 0) {
        const char *host = cfg->memory.redis_host;
        if (!host) host = "localhost";
        unsigned short port = cfg->memory.redis_port;
        if (!port) port = 6379;
        const char *prefix = cfg->memory.redis_key_prefix;
        if (!prefix) prefix = "sc_mem";
        return sc_redis_memory_create(alloc, host, port, prefix);
    }
#endif

    if (strcmp(backend, "api") == 0) {
        const char *base = cfg->memory.api_base_url;
        if (!base) base = "https://api.example.com/memory";
        const char *key = cfg->memory.api_key;
        uint32_t timeout = cfg->memory.api_timeout_ms;
        if (!timeout) timeout = 5000;
        return sc_api_memory_create(alloc, base, key, timeout);
    }

    return sc_markdown_memory_create(alloc, ws);
}

static sc_error_t cmd_service_loop(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "[%s] service loop started\n", SC_CODENAME);

    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    /* ── Create provider ──────────────────────────────────────────────── */
    const char *prov_name = cfg.default_provider ? cfg.default_provider : "openai";
    size_t prov_name_len = strlen(prov_name);
    const char *api_key = sc_config_default_provider_key(&cfg);
    size_t api_key_len = api_key ? strlen(api_key) : 0;
    const char *base_url = sc_config_get_provider_base_url(&cfg, prov_name);
    size_t base_url_len = base_url ? strlen(base_url) : 0;

    sc_provider_t provider;
    err = sc_provider_create(alloc, prov_name, prov_name_len,
        api_key, api_key_len, base_url, base_url_len, &provider);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Provider '%s' init failed: %s\n",
            SC_CODENAME, prov_name, sc_error_string(err));
        sc_config_deinit(&cfg);
        return err;
    }

    /* ── Create runtime, security, memory, tools ──────────────────────── */
    const char *model = cfg.default_model ? cfg.default_model : "";
    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    double temp = cfg.temperature > 0.0 ? cfg.temperature : 0.7;
    uint32_t max_iters = cfg.agent.max_tool_iterations > 0 ? cfg.agent.max_tool_iterations : 25;
    uint32_t max_hist = cfg.agent.max_history_messages > 0 ? cfg.agent.max_history_messages : 100;

    sc_runtime_t runtime;
    err = sc_runtime_from_config(&cfg, &runtime);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Runtime error: %s\n", SC_CODENAME, sc_error_string(err));
        sc_config_deinit(&cfg);
        return err;
    }

    sc_security_policy_t policy = {0};
    uint8_t al = cfg.security.autonomy_level;
    policy.autonomy = (al >= 2) ? SC_AUTONOMY_FULL :
                      (al == 1) ? SC_AUTONOMY_SUPERVISED : SC_AUTONOMY_READ_ONLY;
    policy.workspace_dir = ws;
    policy.workspace_only = true;
    policy.allow_shell = (policy.autonomy != SC_AUTONOMY_READ_ONLY);
    if (runtime.vtable && runtime.vtable->has_shell_access)
        policy.allow_shell = policy.allow_shell && runtime.vtable->has_shell_access(runtime.ctx);
    policy.block_high_risk_commands = (policy.autonomy == SC_AUTONOMY_SUPERVISED);
    policy.require_approval_for_medium_risk = false;
    policy.max_actions_per_hour = 200;
    policy.tracker = sc_rate_tracker_create(alloc, policy.max_actions_per_hour);

    sc_sandbox_alloc_t sb_alloc = {
        .ctx = alloc->ctx, .alloc = alloc->alloc, .free = alloc->free,
    };
    sc_sandbox_storage_t *sb_storage = NULL;
    sc_sandbox_t sandbox = {0};
    sc_net_proxy_t net_proxy = {0};

    if (cfg.security.sandbox_config.enabled ||
        cfg.security.sandbox_config.backend != SC_SANDBOX_NONE) {
        sb_storage = sc_sandbox_storage_create(&sb_alloc);
        if (sb_storage) {
            sandbox = sc_sandbox_create(cfg.security.sandbox_config.backend,
                ws, sb_storage, &sb_alloc);
            if (sandbox.vtable) {
                policy.sandbox = &sandbox;
                if (strcmp(sc_sandbox_name(&sandbox), "firejail") == 0 &&
                    cfg.security.sandbox_config.firejail_args_len > 0) {
                    sc_firejail_sandbox_set_extra_args(
                        (sc_firejail_ctx_t *)sandbox.ctx,
                        (const char *const *)cfg.security.sandbox_config.firejail_args,
                        cfg.security.sandbox_config.firejail_args_len);
                }
            }
        }
    }

    if (cfg.security.sandbox_config.net_proxy.enabled) {
        net_proxy.enabled = true;
        net_proxy.deny_all = cfg.security.sandbox_config.net_proxy.deny_all;
        net_proxy.proxy_addr = cfg.security.sandbox_config.net_proxy.proxy_addr;
        net_proxy.allowed_domains_count = 0;
        for (size_t i = 0; i < cfg.security.sandbox_config.net_proxy.allowed_domains_len
                && i < SC_NET_PROXY_MAX_DOMAINS; i++) {
            net_proxy.allowed_domains[net_proxy.allowed_domains_count++] =
                cfg.security.sandbox_config.net_proxy.allowed_domains[i];
        }
        policy.net_proxy = &net_proxy;
    }

    sc_memory_t memory = service_create_memory(alloc, &cfg, ws);
    sc_session_store_t session_store = {0};
    if (memory.vtable && cfg.memory.backend &&
        strcmp(cfg.memory.backend, "sqlite") == 0)
        session_store = sc_sqlite_memory_get_session_store(&memory);

    sc_cron_scheduler_t *cron = sc_cron_create(alloc, 64, true);

    sc_tool_t *tools = NULL;
    size_t tools_count = 0;
    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,
        memory.vtable ? &memory : NULL, cron, &tools, &tools_count);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Tools init failed: %s\n", SC_CODENAME, sc_error_string(err));
        if (cron) sc_cron_destroy(cron, alloc);
        if (memory.vtable && memory.vtable->deinit) memory.vtable->deinit(memory.ctx);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
        sc_config_deinit(&cfg);
        return err;
    }

    /* ── Create observer from SEACLAW_LOG env ───────────────────────── */
    sc_observer_t observer = {0};
    FILE *log_fp = NULL;
    const char *log_env = getenv("SEACLAW_LOG");
    if (log_env && log_env[0]) {
        log_fp = fopen(log_env, "a");
        if (log_fp)
            observer = sc_log_observer_create(alloc, log_fp);
    }

    /* ── Create agent ─────────────────────────────────────────────────── */
    sc_agent_t agent;
    err = sc_agent_from_config(&agent, alloc, provider,
        tools, tools_count,
        memory.vtable ? &memory : NULL,
        session_store.vtable ? &session_store : NULL,
        observer.vtable ? &observer : NULL, &policy,
        model, strlen(model),
        prov_name, prov_name_len,
        temp, ws, strlen(ws), max_iters, max_hist,
        cfg.memory.auto_save, 2, NULL, 0);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Agent init failed: %s\n", SC_CODENAME, sc_error_string(err));
        if (observer.vtable && observer.vtable->deinit) observer.vtable->deinit(observer.ctx);
        if (log_fp) fclose(log_fp);
        sc_tools_destroy_default(alloc, tools, tools_count);
        if (cron) sc_cron_destroy(cron, alloc);
        if (memory.vtable && memory.vtable->deinit) memory.vtable->deinit(memory.ctx);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
        sc_config_deinit(&cfg);
        return err;
    }

    fprintf(stderr, "[%s] agent ready (provider=%s model=%s tools=%zu)\n",
        SC_CODENAME, prov_name, model[0] ? model : "(default)", tools_count);

    /* ── Wire channels ────────────────────────────────────────────────── */
    sc_service_channel_t channels[4];
    size_t ch_count = 0;

#if SC_HAS_EMAIL
    sc_channel_t email_ch = {0};
    if (cfg.channels.email.smtp_host && cfg.channels.email.from_address) {
        err = sc_email_create(alloc,
            cfg.channels.email.smtp_host, strlen(cfg.channels.email.smtp_host),
            cfg.channels.email.smtp_port ? cfg.channels.email.smtp_port : 587,
            cfg.channels.email.from_address, strlen(cfg.channels.email.from_address),
            &email_ch);
        if (err == SC_OK) {
            if (cfg.channels.email.smtp_user && cfg.channels.email.smtp_pass) {
                sc_email_set_auth(&email_ch,
                    cfg.channels.email.smtp_user, strlen(cfg.channels.email.smtp_user),
                    cfg.channels.email.smtp_pass, strlen(cfg.channels.email.smtp_pass));
            }
            if (cfg.channels.email.imap_host) {
                sc_email_set_imap(&email_ch,
                    cfg.channels.email.imap_host, strlen(cfg.channels.email.imap_host),
                    cfg.channels.email.imap_port ? cfg.channels.email.imap_port : 993);
            }
            channels[ch_count].channel_ctx = email_ch.ctx;
            channels[ch_count].channel = &email_ch;
            channels[ch_count].poll_fn = sc_email_poll;
            channels[ch_count].interval_ms = 30000;
            channels[ch_count].last_poll_ms = 0;
            ch_count++;
            fprintf(stderr, "[%s] email channel configured (%s)\n",
                SC_CODENAME, cfg.channels.email.from_address);
        }
    }
#endif

#if SC_HAS_IMESSAGE
    sc_channel_t imessage_ch = {0};
    if (cfg.channels.imessage.default_target) {
        err = sc_imessage_create(alloc,
            cfg.channels.imessage.default_target,
            strlen(cfg.channels.imessage.default_target),
            &imessage_ch);
        if (err == SC_OK) {
            channels[ch_count].channel_ctx = imessage_ch.ctx;
            channels[ch_count].channel = &imessage_ch;
            channels[ch_count].poll_fn = sc_imessage_poll;
            channels[ch_count].interval_ms = 3000;
            channels[ch_count].last_poll_ms = 0;
            ch_count++;
            fprintf(stderr, "[%s] imessage channel configured (%s)\n",
                SC_CODENAME, cfg.channels.imessage.default_target);
        }
    }
#endif

    fprintf(stderr, "[%s] %zu channel(s) active, cron enabled\n", SC_CODENAME, ch_count);
    err = sc_service_run(alloc, 1000,
        ch_count > 0 ? channels : NULL, ch_count, &agent);

    /* ── Cleanup ──────────────────────────────────────────────────────── */
#if SC_HAS_EMAIL
    if (email_ch.ctx) sc_email_destroy(&email_ch);
#endif
#if SC_HAS_IMESSAGE
    if (imessage_ch.ctx) sc_imessage_destroy(&imessage_ch);
#endif

    sc_agent_deinit(&agent);
    sc_tools_destroy_default(alloc, tools, tools_count);
    if (cron) sc_cron_destroy(cron, alloc);
    if (memory.vtable && memory.vtable->deinit) memory.vtable->deinit(memory.ctx);
    if (observer.vtable && observer.vtable->deinit) observer.vtable->deinit(observer.ctx);
    if (log_fp) fclose(log_fp);
    if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
    if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
    sc_config_deinit(&cfg);
    return err;
}

static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "list";

    if (strcmp(sub, "list") == 0) {
        char dir_path[512];
        size_t dlen = sc_skill_registry_get_installed_dir(dir_path, sizeof(dir_path));
        const char *dir = (dlen > 0) ? dir_path : ".";
        sc_skillforge_t sf;
        sc_error_t err = sc_skillforge_create(alloc, &sf);
        if (err != SC_OK) return err;
        err = sc_skillforge_discover(&sf, dir);
        if (err == SC_OK) {
            sc_skill_t *skills = NULL;
            size_t count = 0;
            sc_skillforge_list_skills(&sf, &skills, &count);
            printf("Installed skills: %zu\n", count);
            for (size_t i = 0; i < count; i++) {
                printf("  - %s (%s) %s\n", skills[i].name, skills[i].description,
                    skills[i].enabled ? "[enabled]" : "[disabled]");
            }
        }
        sc_skillforge_destroy(&sf);
        return err;
    }

    if (strcmp(sub, "search") == 0) {
        const char *query = (argc >= 4 && argv[3]) ? argv[3] : "";
        sc_skill_registry_entry_t *entries = NULL;
        size_t count = 0;
        sc_error_t err = sc_skill_registry_search(alloc, query, &entries, &count);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] skills search: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        printf("Registry matches: %zu\n", count);
        for (size_t i = 0; i < count; i++) {
            printf("  - %s v%s — %s\n",
                entries[i].name, entries[i].version ? entries[i].version : "?",
                entries[i].description ? entries[i].description : "");
        }
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_OK;
    }

    if (strcmp(sub, "install") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills install <name>\n", SC_CODENAME);
            return SC_ERR_INVALID_ARGUMENT;
        }
        sc_error_t err = sc_skill_registry_install(alloc, argv[3]);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] skills install: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        printf("Installed skill: %s\n", argv[3]);
        return SC_OK;
    }

    if (strcmp(sub, "uninstall") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills uninstall <name>\n", SC_CODENAME);
            return SC_ERR_INVALID_ARGUMENT;
        }
        sc_error_t err = sc_skill_registry_uninstall(argv[3]);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] skills uninstall: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        printf("Uninstalled skill: %s\n", argv[3]);
        return SC_OK;
    }

    if (strcmp(sub, "update") == 0) {
        sc_error_t err = sc_skill_registry_update(alloc);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] skills update: %s\n", SC_CODENAME, sc_error_string(err));
            return err;
        }
        printf("Updated all installed skills.\n");
        return SC_OK;
    }

    fprintf(stderr, "[%s] skills: use list, search, install, uninstall, or update\n", SC_CODENAME);
    fprintf(stderr, "  seaclaw skills list\n");
    fprintf(stderr, "  seaclaw skills search <query>\n");
    fprintf(stderr, "  seaclaw skills install <name>\n");
    fprintf(stderr, "  seaclaw skills uninstall <name>\n");
    fprintf(stderr, "  seaclaw skills update\n");
    fprintf(stderr, "  seaclaw skills info <name>\n");
    fprintf(stderr, "  seaclaw skills publish [directory]\n");
    return SC_ERR_INVALID_ARGUMENT;
}

static sc_error_t cmd_migrate(sc_allocator_t *alloc, int argc, char **argv) {
    sc_migration_config_t mc = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_MARKDOWN,
        .source_path = NULL,
        .source_path_len = 0,
        .target_path = ".",
        .target_path_len = 1,
        .dry_run = true,
    };
    if (argc >= 4 && argv[2] && argv[3]) {
        if (strcmp(argv[2], "sqlite") == 0) {
            mc.source = SC_MIGRATION_SOURCE_SQLITE;
            mc.source_path = argv[3];
            mc.source_path_len = strlen(argv[3]);
        } else if (strcmp(argv[2], "markdown") == 0) {
            mc.source = SC_MIGRATION_SOURCE_MARKDOWN;
            mc.source_path = argv[3];
            mc.source_path_len = strlen(argv[3]);
        }
    }
    if (argc >= 3 && argv[2] && mc.source == SC_MIGRATION_SOURCE_NONE && strcmp(argv[2], "--dry-run") != 0) {
        mc.target_path = argv[2];
        mc.target_path_len = strlen(argv[2]);
    }
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(alloc, &mc, &stats, NULL, NULL);
    if (err == SC_OK) {
        printf("Migration: %zu from sqlite, %zu from markdown, %zu imported\n",
            stats.from_sqlite, stats.from_markdown, stats.imported);
    }
    return err;
}

static sc_error_t cmd_mcp(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;

    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    sc_tool_t *tools = NULL;
    size_t tool_count = 0;
    err = sc_tools_create_default(alloc, ".", 1, NULL, &cfg, NULL, NULL,
        &tools, &tool_count);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Tools init error: %s\n", SC_CODENAME, sc_error_string(err));
        sc_config_deinit(&cfg);
        return err;
    }

    sc_mcp_host_t *srv = NULL;
    err = sc_mcp_host_create(alloc, tools, tool_count, &srv);
    if (err != SC_OK) {
        sc_tools_destroy_default(alloc, tools, tool_count);
        sc_config_deinit(&cfg);
        return err;
    }

    err = sc_mcp_host_run(srv);

    sc_mcp_host_destroy(srv);
    sc_tools_destroy_default(alloc, tools, tool_count);
    sc_config_deinit(&cfg);
    return err;
}

static sc_error_t cmd_agent(sc_allocator_t *alloc, int argc, char **argv) {
    return sc_agent_cli_run(alloc, (const char *const *)argv, (size_t)argc);
}

/* Bus→agent bridge: when a SC_BUS_MESSAGE_RECEIVED event arrives from
 * a WebSocket chat.send, feed it into the agent and publish the reply. */
typedef struct gw_agent_bridge {
    sc_agent_t *agent;
    sc_bus_t *bus;
} gw_agent_bridge_t;

static bool gw_agent_on_message(sc_bus_event_type_t type,
    const sc_bus_event_t *ev, void *user_ctx) {
    (void)type;
    gw_agent_bridge_t *b = (gw_agent_bridge_t *)user_ctx;
    if (!b || !b->agent || !ev) return true;
    const char *msg = ev->payload ? (const char *)ev->payload : ev->message;
    if (!msg || !msg[0]) return true;

    char *reply = NULL;
    size_t reply_len = 0;
    sc_error_t err = sc_agent_turn(b->agent, msg, strlen(msg),
        &reply, &reply_len);
    if (err == SC_OK && reply && reply_len > 0) {
        sc_bus_event_t rev;
        memset(&rev, 0, sizeof(rev));
        rev.type = SC_BUS_MESSAGE_SENT;
        snprintf(rev.channel, SC_BUS_CHANNEL_LEN, "%s",
            ev->channel[0] ? ev->channel : "gateway");
        snprintf(rev.id, SC_BUS_ID_LEN, "%s", ev->id);
        rev.payload = reply;
        size_t rl = reply_len;
        if (rl >= SC_BUS_MSG_LEN) rl = SC_BUS_MSG_LEN - 1;
        memcpy(rev.message, reply, rl);
        rev.message[rl] = '\0';
        sc_bus_publish(b->bus, &rev);
    }
    if (reply)
        b->agent->alloc->free(b->agent->alloc->ctx, reply, reply_len + 1);
    return true;
}

static sc_error_t cmd_gateway(sc_allocator_t *alloc, int argc, char **argv) {
    bool with_agent = false;
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-agent") == 0) with_agent = true;
    }

    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    /* ── Build all backend subsystems ──────────────────────────────────── */
    sc_session_manager_t sessions;
    sc_session_manager_init(&sessions, alloc);

    sc_cron_scheduler_t *cron = sc_cron_create(alloc, 64, true);

    sc_skillforge_t skills;
    memset(&skills, 0, sizeof(skills));
    err = sc_skillforge_create(alloc, &skills);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Skillforge init failed: %s\n", SC_CODENAME, sc_error_string(err));
    }

    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    sc_cost_tracker_t costs;
    sc_cost_tracker_init(&costs, alloc, ws, true,
        cfg.cost.daily_limit_usd > 0 ? cfg.cost.daily_limit_usd : 100.0,
        cfg.cost.monthly_limit_usd > 0 ? cfg.cost.monthly_limit_usd : 1000.0,
        cfg.cost.warn_at_percent > 0 ? cfg.cost.warn_at_percent : 80);
    sc_cost_load_history(&costs);

    sc_bus_t bus;
    sc_bus_init(&bus);

    /* ── Tools ─────────────────────────────────────────────────────────── */
    sc_security_policy_t policy = {0};
    uint8_t al = cfg.security.autonomy_level;
    policy.autonomy = (al >= 2) ? SC_AUTONOMY_FULL :
                      (al == 1) ? SC_AUTONOMY_SUPERVISED : SC_AUTONOMY_READ_ONLY;
    policy.workspace_dir = ws;
    policy.workspace_only = true;
    policy.allow_shell = (policy.autonomy != SC_AUTONOMY_READ_ONLY);
    policy.block_high_risk_commands = (policy.autonomy == SC_AUTONOMY_SUPERVISED);
    policy.require_approval_for_medium_risk = false;
    policy.max_actions_per_hour = 200;
    policy.tracker = sc_rate_tracker_create(alloc, policy.max_actions_per_hour);

    sc_sandbox_alloc_t gw_sb_alloc = {
        .ctx = alloc->ctx, .alloc = alloc->alloc, .free = alloc->free,
    };
    sc_sandbox_storage_t *gw_sb_storage = NULL;
    sc_sandbox_t gw_sandbox = {0};
    sc_net_proxy_t gw_net_proxy = {0};

    if (cfg.security.sandbox_config.enabled ||
        cfg.security.sandbox_config.backend != SC_SANDBOX_NONE) {
        gw_sb_storage = sc_sandbox_storage_create(&gw_sb_alloc);
        if (gw_sb_storage) {
            gw_sandbox = sc_sandbox_create(cfg.security.sandbox_config.backend,
                ws, gw_sb_storage, &gw_sb_alloc);
            if (gw_sandbox.vtable) {
                policy.sandbox = &gw_sandbox;
                if (strcmp(sc_sandbox_name(&gw_sandbox), "firejail") == 0 &&
                    cfg.security.sandbox_config.firejail_args_len > 0) {
                    sc_firejail_sandbox_set_extra_args(
                        (sc_firejail_ctx_t *)gw_sandbox.ctx,
                        (const char *const *)cfg.security.sandbox_config.firejail_args,
                        cfg.security.sandbox_config.firejail_args_len);
                }
            }
        }
    }

    if (cfg.security.sandbox_config.net_proxy.enabled) {
        gw_net_proxy.enabled = true;
        gw_net_proxy.deny_all = cfg.security.sandbox_config.net_proxy.deny_all;
        gw_net_proxy.proxy_addr = cfg.security.sandbox_config.net_proxy.proxy_addr;
        gw_net_proxy.allowed_domains_count = 0;
        for (size_t i = 0; i < cfg.security.sandbox_config.net_proxy.allowed_domains_len
                && i < SC_NET_PROXY_MAX_DOMAINS; i++) {
            gw_net_proxy.allowed_domains[gw_net_proxy.allowed_domains_count++] =
                cfg.security.sandbox_config.net_proxy.allowed_domains[i];
        }
        policy.net_proxy = &gw_net_proxy;
    }

    sc_tool_t *tools = NULL;
    size_t tools_count = 0;
    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,
        NULL, cron, &tools, &tools_count);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Tools init failed: %s\n", SC_CODENAME, sc_error_string(err));
        sc_cost_tracker_deinit(&costs);
        sc_session_manager_deinit(&sessions);
        if (cron) sc_cron_destroy(cron, alloc);
        sc_skillforge_destroy(&skills);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        if (gw_sb_storage) sc_sandbox_storage_destroy(gw_sb_storage, &gw_sb_alloc);
        sc_config_deinit(&cfg);
        return err;
    }

    /* ── App context ───────────────────────────────────────────────────── */
    sc_app_context_t app_ctx = {
        .config = &cfg,
        .alloc = alloc,
        .sessions = &sessions,
        .cron = cron,
        .skills = &skills,
        .costs = &costs,
        .bus = &bus,
        .tools = tools,
        .tools_count = tools_count,
    };

    /* ── Gateway config ────────────────────────────────────────────────── */
    sc_gateway_config_t gw_config;
    sc_gateway_config_from_cfg(&cfg.gateway, &gw_config);
    gw_config.app_ctx = &app_ctx;

    /* ── Optional agent integration ────────────────────────────────────── */
    sc_provider_t provider = {0};
    sc_agent_t agent = {0};
    sc_memory_t memory = {0};
    gw_agent_bridge_t agent_bridge = {0};
    bool agent_active = false;

    if (with_agent) {
        const char *prov_name = cfg.default_provider ? cfg.default_provider : "openai";
        size_t prov_name_len = strlen(prov_name);
        const char *api_key = sc_config_default_provider_key(&cfg);
        size_t api_key_len = api_key ? strlen(api_key) : 0;
        const char *base_url = sc_config_get_provider_base_url(&cfg, prov_name);
        size_t base_url_len = base_url ? strlen(base_url) : 0;

        err = sc_provider_create(alloc, prov_name, prov_name_len,
            api_key, api_key_len, base_url, base_url_len, &provider);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] Provider '%s' init failed: %s\n",
                SC_CODENAME, prov_name, sc_error_string(err));
            goto cleanup;
        }

        const char *model = cfg.default_model ? cfg.default_model : "";
        double temp = cfg.temperature > 0.0 ? cfg.temperature : 0.7;
        uint32_t max_iters = cfg.agent.max_tool_iterations > 0 ? cfg.agent.max_tool_iterations : 25;
        uint32_t max_hist = cfg.agent.max_history_messages > 0 ? cfg.agent.max_history_messages : 100;

        memory = service_create_memory(alloc, &cfg, ws);

        err = sc_agent_from_config(&agent, alloc, provider,
            tools, tools_count,
            memory.vtable ? &memory : NULL,
            NULL, NULL, &policy,
            model, strlen(model),
            prov_name, prov_name_len,
            temp, ws, strlen(ws), max_iters, max_hist,
            cfg.memory.auto_save, 2, NULL, 0);
        if (err != SC_OK) {
            fprintf(stderr, "[%s] Agent init failed: %s\n", SC_CODENAME, sc_error_string(err));
            goto cleanup;
        }
        agent_active = true;

        agent_bridge.agent = &agent;
        agent_bridge.bus = &bus;
        sc_bus_subscribe(&bus, gw_agent_on_message, &agent_bridge,
            SC_BUS_MESSAGE_RECEIVED);

        fprintf(stderr, "[%s] gateway+agent mode (provider=%s tools=%zu)\n",
            SC_CODENAME, prov_name, tools_count);
    } else {
        fprintf(stderr, "[%s] gateway-only mode (use --with-agent for full agent)\n",
            SC_CODENAME);
    }

    /* ── Run gateway (blocks) ──────────────────────────────────────────── */
    err = sc_gateway_run(alloc, gw_config.host, gw_config.port, &gw_config);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Gateway error: %s\n", SC_CODENAME, sc_error_string(err));
    }

cleanup:
    if (agent_active) {
        sc_bus_unsubscribe(&bus, gw_agent_on_message, &agent_bridge);
        sc_agent_deinit(&agent);
    }
    if (memory.vtable && memory.vtable->deinit) memory.vtable->deinit(memory.ctx);
    sc_tools_destroy_default(alloc, tools, tools_count);
    if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
    if (gw_sb_storage) sc_sandbox_storage_destroy(gw_sb_storage, &gw_sb_alloc);
    sc_cost_tracker_deinit(&costs);
    sc_session_manager_deinit(&sessions);
    if (cron) sc_cron_destroy(cron, alloc);
    sc_skillforge_destroy(&skills);
    sc_config_deinit(&cfg);
    return err;
}

static int run_command(sc_allocator_t *alloc, int argc, char **argv, sc_command_t const *cmd) {
    sc_error_t err = cmd->handler(alloc, argc, argv);
    if (err == SC_OK) return 0;
    return 1;
}

int main(int argc, char *argv[]) {
    sc_allocator_t alloc = sc_system_allocator();

    if (argc >= 2) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            return run_command(&alloc, argc, argv, find_command("version")) == 0 ? 0 : 1;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            return run_command(&alloc, argc, argv, find_command("help")) == 0 ? 0 : 1;
        }
        if (strcmp(argv[1], "--mcp") == 0) {
            return run_command(&alloc, argc, argv, find_command("mcp")) == 0 ? 0 : 1;
        }
    }

    const char *cmd_name = (argc >= 2 && argv[1]) ? argv[1] : "agent";
    sc_command_t const *cmd = find_command(cmd_name);

    if (!cmd) {
        fprintf(stderr, "Unknown command: %s\n", cmd_name);
        fprintf(stderr, "Run 'seaclaw help' for usage.\n");
        return 1;
    }

    return run_command(&alloc, argc, argv, cmd) == 0 ? 0 : 1;
}
