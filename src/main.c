#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#include "seaclaw/agent.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/cli.h"
#include "seaclaw/agent/episodic.h"
#include "seaclaw/agent/outcomes.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/bootstrap.h"
#include "seaclaw/bus.h"
#include "seaclaw/channel.h"
#include "seaclaw/channels/thread_binding.h"
#include "seaclaw/cli_commands.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/cost.h"
#include "seaclaw/cron.h"
#include "seaclaw/crontab.h"
#include "seaclaw/daemon.h"
#include "seaclaw/doctor.h"
#include "seaclaw/gateway.h"
#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/health.h"
#include "seaclaw/mcp_server.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/memory/factory.h"
#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory/vector.h"
#include "seaclaw/migration.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/onboard.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/runtime.h"
#include "seaclaw/security.h"
#include "seaclaw/security/audit.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/session.h"
#include "seaclaw/skill_registry.h"
#ifdef SC_HAS_SKILLS
#include "seaclaw/skillforge.h"
#endif
#ifdef SC_HAS_PERSONA
#include "seaclaw/persona.h"
#endif
#include "seaclaw/plugin_loader.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/factory.h"
#if SC_HAS_EMAIL
#include "seaclaw/channels/email.h"
#endif
#if SC_HAS_IMESSAGE
#include "seaclaw/channels/imessage.h"
#endif
#if SC_HAS_IMAP
#include "seaclaw/channels/imap.h"
#endif
#if SC_HAS_GMAIL
#include "seaclaw/channels/gmail.h"
#endif
#if SC_HAS_TELEGRAM
#include "seaclaw/channels/telegram.h"
#endif
#if SC_HAS_DISCORD
#include "seaclaw/channels/discord.h"
#endif
#if SC_HAS_SLACK
#include "seaclaw/channels/slack.h"
#endif
#if SC_HAS_WHATSAPP
#include "seaclaw/channels/whatsapp.h"
#endif
#if SC_HAS_FACEBOOK
#include "seaclaw/channels/facebook.h"
#endif
#if SC_HAS_LINE
#include "seaclaw/channels/line.h"
#endif
#if SC_HAS_LARK
#include "seaclaw/channels/lark.h"
#endif
#if SC_HAS_GOOGLE_CHAT
#include "seaclaw/channels/google_chat.h"
#endif
#if SC_HAS_DINGTALK
#include "seaclaw/channels/dingtalk.h"
#endif
#if SC_HAS_TEAMS
#include "seaclaw/channels/teams.h"
#endif
#if SC_HAS_TWILIO
#include "seaclaw/channels/twilio.h"
#endif
#if SC_HAS_ONEBOT
#include "seaclaw/channels/onebot.h"
#endif
#if SC_HAS_QQ
#include "seaclaw/channels/qq.h"
#endif
#if SC_HAS_INSTAGRAM
#include "seaclaw/channels/instagram.h"
#endif
#if SC_HAS_TWITTER
#include "seaclaw/channels/twitter.h"
#endif
#if SC_HAS_GOOGLE_RCS
#include "seaclaw/channels/google_rcs.h"
#endif
#if SC_HAS_MQTT
#include "seaclaw/channels/mqtt.h"
#endif
#if SC_HAS_MATRIX
#include "seaclaw/channels/matrix.h"
#endif
#if SC_HAS_IRC
#include "seaclaw/channels/irc.h"
#endif
#if SC_HAS_NOSTR
#include "seaclaw/channels/nostr.h"
#endif

#define SC_VERSION  "0.3.0"
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
#ifdef SC_HAS_CRON
static sc_error_t cmd_cron(sc_allocator_t *alloc, int argc, char **argv);
#endif
static sc_error_t cmd_onboard(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_service(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_service_loop(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_migrate(sc_allocator_t *alloc, int argc, char **argv);
#ifdef SC_HAS_PERSONA
static sc_error_t cmd_persona(sc_allocator_t *alloc, int argc, char **argv);
#endif

/* Forward declarations for gateway→agent bridge (used by both service-loop and gateway) */
typedef struct gw_agent_bridge {
    sc_agent_t *agent;
    sc_bus_t *bus;
    sc_thread_binding_t *thread_binding;
} gw_agent_bridge_t;

static bool gw_agent_on_message(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *user_ctx);

static pthread_mutex_t svc_agent_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool svc_agent_on_message_locked(sc_bus_event_type_t type, const sc_bus_event_t *ev,
                                        void *user_ctx) {
    pthread_mutex_lock(&svc_agent_mutex);
    bool result = gw_agent_on_message(type, ev, user_ctx);
    pthread_mutex_unlock(&svc_agent_mutex);
    return result;
}

static const sc_command_t commands[] = {
    {"agent", "Start interactive agent (--demo: use local Ollama)", cmd_agent},
    {"init", "Initialize config file", cmd_init},
    {"gateway", "Start webhook gateway server", cmd_gateway},
    {"mcp", "Run as MCP server (stdin/stdout JSON-RPC)", cmd_mcp},
    {"service", "Run as background service (daemonize)", cmd_service},
    {"service-loop", "Run service loop in foreground", cmd_service_loop},
    {"status", "Show runtime status", cmd_status},
    {"onboard", "Interactive setup wizard", cmd_onboard},
    {"doctor", "Run system diagnostics", cmd_doctor},
#ifdef SC_HAS_CRON
    {"cron", "Manage scheduled tasks", cmd_cron},
#endif
    {"channel", "Channel management", cmd_channel},
    {"skills", "Skill discovery and integration", cmd_skills},
    {"hardware", "Hardware peripheral management", cmd_hardware},
    {"sandbox", "Show sandbox status and backends", cmd_sandbox},
    {"migrate", "Migrate memory backends", cmd_migrate},
    {"memory", "Memory operations", cmd_memory},
#ifdef SC_HAS_PERSONA
    {"persona", "Create and manage persona profiles", cmd_persona},
#endif
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
    fprintf(out, "%s v%s — Autonomous AI Assistant Runtime (C/ASM/WASM)\n\n", SC_CODENAME,
            SC_VERSION);
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
            printf("  %s: %s\n", r.checks[i].name, r.checks[i].healthy ? "ok" : "error");
            if (r.checks[i].message)
                printf("    %s\n", r.checks[i].message);
        }
        alloc->free(alloc->ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
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
            const char *sev = items[i].severity == SC_DIAG_ERR    ? "error"
                              : items[i].severity == SC_DIAG_WARN ? "warning"
                                                                  : "info";
            printf("[doctor] %s: %s — %s\n", sev, items[i].category ? items[i].category : "config",
                   items[i].message ? items[i].message : "");
        }
    }
    if (items) {
        for (size_t i = 0; i < item_count; i++) {
            if (items[i].category)
                alloc->free(alloc->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
            if (items[i].message)
                alloc->free(alloc->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
        }
        alloc->free(alloc->ctx, items, item_count * sizeof(sc_diag_item_t));
    }

    sc_config_deinit(&cfg);
    return SC_OK;
}

#ifdef SC_HAS_CRON
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
                printf("%s  %s  %s  %s\n", entries[i].id, entries[i].schedule, entries[i].command,
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
            if (i > 4)
                cmd_len++;
            cmd_len += strlen(argv[i]);
        }
        char *command = (char *)alloc->alloc(alloc->ctx, cmd_len + 1);
        if (!command) {
            alloc->free(alloc->ctx, path, path_len + 1);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t pos = 0;
        for (int i = 4; i < argc && argv[i]; i++) {
            if (i > 4)
                command[pos++] = ' ';
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
        if (new_id)
            alloc->free(alloc->ctx, new_id, strlen(new_id) + 1);
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
#endif /* SC_HAS_CRON */

static sc_error_t cmd_onboard(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return sc_onboard_run(alloc);
}

static sc_error_t cmd_service(sc_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "start";

    if (strcmp(sub, "start") == 0) {
        sc_error_t err = sc_daemon_start();
        if (err == SC_ERR_NOT_SUPPORTED)
            fprintf(stderr, "[%s] daemon not supported on this platform\n", SC_CODENAME);
        else if (err == SC_OK)
            printf("[%s] service started\n", SC_CODENAME);
        else
            fprintf(stderr, "[%s] failed to start service: %s\n", SC_CODENAME,
                    sc_error_string(err));
        return err;
    }

    if (strcmp(sub, "stop") == 0) {
        sc_error_t err = sc_daemon_stop();
        if (err == SC_OK)
            printf("[%s] service stopped\n", SC_CODENAME);
        else if (err == SC_ERR_NOT_FOUND)
            fprintf(stderr, "[%s] service is not running\n", SC_CODENAME);
        else
            fprintf(stderr, "[%s] failed to stop service: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    if (strcmp(sub, "status") == 0) {
        bool running = sc_daemon_status();
        printf("[%s] service is %s\n", SC_CODENAME, running ? "running" : "stopped");
        return SC_OK;
    }

    if (strcmp(sub, "install") == 0) {
        sc_error_t err = sc_daemon_install(alloc);
        if (err == SC_OK)
            printf("[%s] service installed and started\n", SC_CODENAME);
        else
            fprintf(stderr, "[%s] install failed: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    if (strcmp(sub, "uninstall") == 0) {
        sc_error_t err = sc_daemon_uninstall();
        if (err == SC_OK)
            printf("[%s] service uninstalled\n", SC_CODENAME);
        else
            fprintf(stderr, "[%s] uninstall failed: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    if (strcmp(sub, "logs") == 0)
        return sc_daemon_logs();

    fprintf(stderr, "[%s] unknown service subcommand: %s\n", SC_CODENAME, sub);
    fprintf(stderr, "Usage: %s service [start|stop|status|install|uninstall|logs]\n",
            argv[0] ? argv[0] : "seaclaw");
    return SC_ERR_INVALID_ARGUMENT;
}

/* Gateway thread context for --with-gateway mode */
typedef struct svc_gw_thread_ctx {
    sc_allocator_t *alloc;
    sc_gateway_config_t config;
    const char *host;
    uint16_t port;
} svc_gw_thread_ctx_t;

static void *svc_gateway_thread(void *arg) {
    svc_gw_thread_ctx_t *ctx = (svc_gw_thread_ctx_t *)arg;
    sc_error_t err = sc_gateway_run(ctx->alloc, ctx->host, ctx->port, &ctx->config);
    if (err != SC_OK)
        fprintf(stderr, "[seaclaw] gateway thread error: %s\n", sc_error_string(err));
    return NULL;
}

/* Webhook dispatcher — routes gateway webhook POSTs to the correct channel handler */
typedef struct webhook_dispatcher_ctx {
    sc_allocator_t *alloc;
    sc_service_channel_t *channels;
    size_t ch_count;
} webhook_dispatcher_ctx_t;

static void webhook_dispatcher(const char *channel, const char *body, size_t body_len, void *ctx) {
    webhook_dispatcher_ctx_t *d = (webhook_dispatcher_ctx_t *)ctx;
    if (!d || !channel || !body)
        return;
    for (size_t i = 0; i < d->ch_count; i++) {
        const char *name = d->channels[i].channel->vtable->name(d->channels[i].channel_ctx);
        if (!name || strcmp(name, channel) != 0)
            continue;
        if (d->channels[i].webhook_fn)
            d->channels[i].webhook_fn(d->channels[i].channel_ctx, d->alloc, body, body_len);
        return;
    }
}

static sc_error_t cmd_service_loop(sc_allocator_t *alloc, int argc, char **argv) {
    bool with_gateway = false;
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-gateway") == 0)
            with_gateway = true;
    }
    fprintf(stderr, "[%s] service loop started%s\n", SC_CODENAME,
            with_gateway ? " (with gateway)" : "");

    sc_app_ctx_t app_ctx;
    sc_error_t err = sc_app_bootstrap(&app_ctx, alloc, NULL, true, true);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Bootstrap failed: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    sc_bus_t svc_bus;
    sc_bus_init(&svc_bus);
    sc_awareness_t svc_awareness = {0};
    (void)sc_awareness_init(&svc_awareness, &svc_bus);
    if (svc_awareness.bus && app_ctx.agent)
        sc_agent_set_awareness(app_ctx.agent, (struct sc_awareness *)&svc_awareness);

    const char *prov_name =
        app_ctx.cfg->default_provider ? app_ctx.cfg->default_provider : "openai";
    const char *model = app_ctx.cfg->default_model ? app_ctx.cfg->default_model : "";
    fprintf(stderr, "[%s] agent ready (provider=%s model=%s tools=%zu)\n", SC_CODENAME, prov_name,
            model[0] ? model : "(default)", app_ctx.tools_count);

#ifdef SC_HAS_CRON
    fprintf(stderr, "[%s] %zu channel(s) active, cron enabled\n", SC_CODENAME,
            app_ctx.channel_count);

    /* Register proactive engagement cron jobs from persona contacts */
#ifdef SC_HAS_PERSONA
    if (app_ctx.agent && app_ctx.agent->persona && app_ctx.agent->scheduler) {
        const sc_persona_t *persona = app_ctx.agent->persona;
        for (size_t ci = 0; ci < persona->contacts_count; ci++) {
            const sc_contact_profile_t *cp = &persona->contacts[ci];
            if (!cp->proactive_checkin || !cp->proactive_channel)
                continue;
            const char *sched = cp->proactive_schedule ? cp->proactive_schedule : "0 10 * * *";

            char prompt[512];
            snprintf(prompt, sizeof(prompt),
                     "You are checking in with %s (%s). Based on your relationship and "
                     "recent conversations, send a brief, natural check-in message. "
                     "Follow the NOTICE>WAIT>BRIDGE>OFFER>INVITE pattern: notice something "
                     "relevant, bridge to a shared interest, optionally offer or invite. "
                     "Keep it 1-2 sentences. If you have nothing meaningful to say, respond "
                     "with exactly 'SKIP' and nothing else.",
                     cp->name ? cp->name : cp->contact_id, cp->contact_id);

            char job_name[128];
            snprintf(job_name, sizeof(job_name), "proactive:%s",
                     cp->name ? cp->name : cp->contact_id);

            /* Encode target as "channel:contact_id" for directed sends */
            char channel_target[192];
            snprintf(channel_target, sizeof(channel_target), "%s:%s", cp->proactive_channel,
                     cp->contact_id);

            uint64_t job_id = 0;
            sc_error_t jerr =
                sc_cron_add_agent_job((sc_cron_scheduler_t *)app_ctx.agent->scheduler, alloc, sched,
                                      prompt, channel_target, job_name, &job_id);
            if (jerr == SC_OK) {
                fprintf(stderr, "[%s] proactive check-in registered for %s (id=%llu sched=%s)\n",
                        SC_CODENAME, cp->name ? cp->name : cp->contact_id,
                        (unsigned long long)job_id, sched);
            }
        }
    }
#endif
#else
    fprintf(stderr, "[%s] %zu channel(s) active\n", SC_CODENAME, app_ctx.channel_count);
#endif

    /* ── Optional gateway on background thread ─────────────────────────── */
    pthread_t gw_tid = 0;
    svc_gw_thread_ctx_t gw_tctx;
    memset(&gw_tctx, 0, sizeof(gw_tctx));
    sc_gateway_config_t gw_config;
    memset(&gw_config, 0, sizeof(gw_config));
    sc_app_context_t svc_app_ctx;
    memset(&svc_app_ctx, 0, sizeof(svc_app_ctx));
    gw_agent_bridge_t svc_agent_bridge = {0};
    if (with_gateway) {
        sc_gateway_config_from_cfg(&app_ctx.cfg->gateway, &gw_config);

        svc_app_ctx.config = app_ctx.cfg;
        svc_app_ctx.alloc = alloc;
        svc_app_ctx.tools = app_ctx.tools;
        svc_app_ctx.tools_count = app_ctx.tools_count;
        svc_app_ctx.bus = &svc_bus;
        svc_app_ctx.agent = app_ctx.agent;
        gw_config.app_ctx = &svc_app_ctx;

        static webhook_dispatcher_ctx_t wh_ctx;
        wh_ctx.alloc = alloc;
        wh_ctx.channels = app_ctx.channels;
        wh_ctx.ch_count = app_ctx.channel_count;
        gw_config.on_webhook = webhook_dispatcher;
        gw_config.on_webhook_ctx = &wh_ctx;

        gw_tctx.alloc = alloc;
        gw_tctx.config = gw_config;
        gw_tctx.host = app_ctx.cfg->gateway.host ? app_ctx.cfg->gateway.host : "127.0.0.1";
        gw_tctx.port = app_ctx.cfg->gateway.port > 0 ? app_ctx.cfg->gateway.port : 3000;

        svc_agent_bridge.agent = app_ctx.agent;
        svc_agent_bridge.bus = &svc_bus;
        svc_agent_bridge.thread_binding = NULL;
        sc_bus_subscribe(&svc_bus, svc_agent_on_message_locked, &svc_agent_bridge,
                         SC_BUS_MESSAGE_RECEIVED);

        if (pthread_create(&gw_tid, NULL, svc_gateway_thread, &gw_tctx) == 0) {
            fprintf(stderr, "[%s] gateway listening on %s:%u\n", SC_CODENAME, gw_tctx.host,
                    gw_tctx.port);
        } else {
            fprintf(stderr, "[%s] warning: failed to start gateway thread\n", SC_CODENAME);
            gw_tid = 0;
        }
    }

    err = sc_service_run(alloc, 1000, app_ctx.channel_count > 0 ? app_ctx.channels : NULL,
                         app_ctx.channel_count, app_ctx.agent, app_ctx.cfg);

    if (with_gateway) {
        sc_bus_unsubscribe(&svc_bus, svc_agent_on_message_locked, &svc_agent_bridge);
        if (gw_tid) {
            pthread_join(gw_tid, NULL);
        }
    }

    sc_awareness_deinit(&svc_awareness);
    sc_app_teardown(&app_ctx);
    sc_plugin_unload_all();
    return err;
}

#ifdef SC_HAS_SKILLS
static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "list";

    if (strcmp(sub, "list") == 0) {
        char dir_path[512];
        size_t dlen = sc_skill_registry_get_installed_dir(dir_path, sizeof(dir_path));
        const char *dir = (dlen > 0) ? dir_path : ".";
        sc_skillforge_t sf;
        sc_error_t err = sc_skillforge_create(alloc, &sf);
        if (err != SC_OK)
            return err;
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
            printf("  - %s v%s — %s\n", entries[i].name,
                   entries[i].version ? entries[i].version : "?",
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
#else
static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    fprintf(stderr, "[%s] skills support not built (compile with SC_ENABLE_SKILLS=ON)\n",
            SC_CODENAME);
    return SC_ERR_NOT_SUPPORTED;
}
#endif

#ifdef SC_HAS_PERSONA
static sc_error_t cmd_persona(sc_allocator_t *alloc, int argc, char **argv) {
    sc_persona_cli_args_t args;
    sc_error_t err = sc_persona_cli_parse(argc, (const char **)argv, &args);
    if (err != SC_OK) {
        fprintf(
            stderr,
            "Usage: seaclaw persona <create|update|show|list|delete|validate|export|merge|import> "
            "[name] [options]\n");
        fprintf(
            stderr,
            "  create <name> [--from-imessage] [--from-gmail] [--from-facebook] [--interactive]\n");
        fprintf(stderr, "  update <name> [--from-imessage] [--from-gmail] [--from-facebook]\n");
        fprintf(stderr, "  show <name>\n");
        fprintf(stderr, "  list\n");
        fprintf(stderr, "  delete <name>\n");
        fprintf(stderr, "  validate <name>\n");
        fprintf(stderr, "  export <name>\n");
        fprintf(stderr, "  merge <output_name> <name1> <name2> [name3...]\n");
        fprintf(stderr, "  import <name> [--from-stdin | --from-file <path>]\n");
        return err;
    }
    return sc_persona_cli_run(alloc, &args);
}
#endif

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
    if (argc >= 3 && argv[2] && mc.source == SC_MIGRATION_SOURCE_NONE &&
        strcmp(argv[2], "--dry-run") != 0) {
        mc.target_path = argv[2];
        mc.target_path_len = strlen(argv[2]);
    }
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(alloc, &mc, &stats, NULL, NULL);
    if (err == SC_OK) {
        printf("Migration: %zu from sqlite, %zu from markdown, %zu imported\n", stats.from_sqlite,
               stats.from_markdown, stats.imported);
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

    sc_agent_pool_t *mcp_pool = sc_agent_pool_create(alloc, 8);

    sc_tool_t *tools = NULL;
    size_t tool_count = 0;
    err = sc_tools_create_default(alloc, ".", 1, NULL, &cfg, NULL, NULL, mcp_pool, NULL, &tools,
                                  &tool_count);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Tools init error: %s\n", SC_CODENAME, sc_error_string(err));
        if (mcp_pool)
            sc_agent_pool_destroy(mcp_pool);
        sc_config_deinit(&cfg);
        return err;
    }

    sc_mcp_host_t *srv = NULL;
    err = sc_mcp_host_create(alloc, tools, tool_count, NULL, &srv);
    if (err != SC_OK) {
        sc_tools_destroy_default(alloc, tools, tool_count);
        if (mcp_pool)
            sc_agent_pool_destroy(mcp_pool);
        sc_config_deinit(&cfg);
        return err;
    }

    err = sc_mcp_host_run(srv);

    sc_mcp_host_destroy(srv);
    sc_tools_destroy_default(alloc, tools, tool_count);
    if (mcp_pool)
        sc_agent_pool_destroy(mcp_pool);
    sc_config_deinit(&cfg);
    return err;
}

static sc_error_t cmd_agent(sc_allocator_t *alloc, int argc, char **argv) {
    return sc_agent_cli_run(alloc, (const char *const *)argv, (size_t)argc);
}

/* Bus→agent bridge: streaming context and callback implementation. */
typedef struct gw_stream_ctx {
    sc_bus_t *bus;
    char channel[SC_BUS_CHANNEL_LEN];
    char id[SC_BUS_ID_LEN];
} gw_stream_ctx_t;

static void gw_stream_token_cb(const char *delta, size_t len, void *ctx) {
    gw_stream_ctx_t *sc = (gw_stream_ctx_t *)ctx;
    if (!sc || !delta || !len)
        return;
    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC_BUS_MESSAGE_CHUNK;
    memcpy(ev.channel, sc->channel, SC_BUS_CHANNEL_LEN);
    memcpy(ev.id, sc->id, SC_BUS_ID_LEN);
    size_t copy_len = len < SC_BUS_MSG_LEN - 1 ? len : SC_BUS_MSG_LEN - 1;
    memcpy(ev.message, delta, copy_len);
    ev.message[copy_len] = '\0';
    sc_bus_publish(sc->bus, &ev);
}

static bool gw_agent_on_message(sc_bus_event_type_t type, const sc_bus_event_t *ev,
                                void *user_ctx) {
    (void)type;
    gw_agent_bridge_t *b = (gw_agent_bridge_t *)user_ctx;
    if (!b || !b->agent || !ev)
        return true;
    const char *msg = ev->payload ? (const char *)ev->payload : ev->message;
    if (!msg || !msg[0])
        return true;

    char *reply = NULL;
    size_t reply_len = 0;
    b->agent->active_channel = "gateway";
    b->agent->active_channel_len = 8;
    gw_stream_ctx_t stream_ctx;
    memset(&stream_ctx, 0, sizeof(stream_ctx));
    stream_ctx.bus = b->bus;
    snprintf(stream_ctx.channel, SC_BUS_CHANNEL_LEN, "%s",
             ev->channel[0] ? ev->channel : "gateway");
    snprintf(stream_ctx.id, SC_BUS_ID_LEN, "%s", ev->id);
    sc_error_t err = sc_agent_turn_stream(b->agent, msg, strlen(msg), gw_stream_token_cb,
                                          &stream_ctx, &reply, &reply_len);
    if (err == SC_OK && reply && reply_len > 0) {
        sc_bus_event_t rev;
        memset(&rev, 0, sizeof(rev));
        rev.type = SC_BUS_MESSAGE_SENT;
        int nc = snprintf(rev.channel, SC_BUS_CHANNEL_LEN, "%s",
                          ev->channel[0] ? ev->channel : "gateway");
        if (nc < 0 || (size_t)nc >= SC_BUS_CHANNEL_LEN)
            (void)snprintf(rev.channel, SC_BUS_CHANNEL_LEN, "gateway");
        int ni = snprintf(rev.id, SC_BUS_ID_LEN, "%s", ev->id);
        if (ni < 0 || (size_t)ni >= SC_BUS_ID_LEN)
            rev.id[0] = '\0';
        rev.payload = reply;
        size_t rl = reply_len;
        if (rl >= SC_BUS_MSG_LEN)
            rl = SC_BUS_MSG_LEN - 1;
        memcpy(rev.message, reply, rl);
        rev.message[rl] = '\0';
        sc_bus_publish(b->bus, &rev);
    } else if (err != SC_OK) {
        fprintf(stderr, "[gateway] agent_turn error: %s\n", sc_error_string(err));
        sc_bus_event_t eev;
        memset(&eev, 0, sizeof(eev));
        eev.type = SC_BUS_ERROR;
        int ec = snprintf(eev.channel, SC_BUS_CHANNEL_LEN, "%s",
                          ev->channel[0] ? ev->channel : "gateway");
        if (ec < 0 || (size_t)ec >= SC_BUS_CHANNEL_LEN)
            (void)snprintf(eev.channel, SC_BUS_CHANNEL_LEN, "gateway");
        int ei = snprintf(eev.id, SC_BUS_ID_LEN, "%s", ev->id);
        if (ei < 0 || (size_t)ei >= SC_BUS_ID_LEN)
            eev.id[0] = '\0';
        const char *emsg = sc_error_string(err);
        size_t el = strlen(emsg);
        if (el >= SC_BUS_MSG_LEN)
            el = SC_BUS_MSG_LEN - 1;
        memcpy(eev.message, emsg, el);
        eev.message[el] = '\0';
        sc_bus_publish(b->bus, &eev);
    }
    if (reply)
        b->agent->alloc->free(b->agent->alloc->ctx, reply, reply_len + 1);
    return true;
}

static sc_error_t cmd_gateway(sc_allocator_t *alloc, int argc, char **argv) {
    bool with_agent = false;
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-agent") == 0)
            with_agent = true;
    }

    sc_app_ctx_t app;
    sc_error_t err = sc_app_bootstrap(&app, alloc, NULL, with_agent, false);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Bootstrap failed: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    const char *ws = app.cfg->workspace_dir ? app.cfg->workspace_dir : ".";

    /* ── Gateway-specific subsystems (not in bootstrap) ──────────────────── */
    sc_session_manager_t sessions;
    sc_session_manager_init(&sessions, alloc);

    sc_bus_t bus;
    sc_bus_init(&bus);

    sc_cost_tracker_t costs;
    sc_cost_tracker_init(&costs, alloc, ws, true,
                         app.cfg->cost.daily_limit_usd > 0 ? app.cfg->cost.daily_limit_usd : 100.0,
                         app.cfg->cost.monthly_limit_usd > 0 ? app.cfg->cost.monthly_limit_usd
                                                             : 1000.0,
                         app.cfg->cost.warn_at_percent > 0 ? app.cfg->cost.warn_at_percent : 80);
    sc_cost_load_history(&costs);

#ifdef SC_HAS_SKILLS
    sc_skillforge_t skills;
    memset(&skills, 0, sizeof(skills));
    err = sc_skillforge_create(alloc, &skills);
    if (err != SC_OK)
        fprintf(stderr, "[%s] Skillforge init failed: %s\n", SC_CODENAME, sc_error_string(err));
#endif

    /* Load crontab into bootstrap's cron (bootstrap creates cron but does not load file) */
#ifdef SC_HAS_CRON
    if (app.cron) {
        char *cron_path = NULL;
        size_t cron_path_len = 0;
        if (sc_crontab_get_path(alloc, &cron_path, &cron_path_len) == SC_OK) {
            sc_crontab_entry_t *entries = NULL;
            size_t entry_count = 0;
            if (sc_crontab_load(alloc, cron_path, &entries, &entry_count) == SC_OK) {
                for (size_t ci = 0; ci < entry_count; ci++) {
                    if (entries[ci].enabled && entries[ci].schedule && entries[ci].command) {
                        uint64_t unused_id = 0;
                        sc_cron_add_job((sc_cron_scheduler_t *)app.cron, alloc,
                                        entries[ci].schedule, entries[ci].command, entries[ci].id,
                                        &unused_id);
                    }
                }
                sc_crontab_entries_free(alloc, entries, entry_count);
            }
            alloc->free(alloc->ctx, cron_path, cron_path_len + 1);
        }
    }
#endif

    sc_thread_binding_t *gw_thread_binding =
        with_agent ? sc_thread_binding_create(alloc, 64) : NULL;
    gw_agent_bridge_t agent_bridge = {0};
    sc_awareness_t gw_awareness = {0};

    /* ── Gateway app context (sc_app_context_t for RPC handlers) ───────── */
    sc_app_context_t gw_app_ctx = {
        .config = app.cfg,
        .alloc = alloc,
        .sessions = &sessions,
        .cron = app.cron,
#ifdef SC_HAS_SKILLS
        .skills = &skills,
#else
        .skills = NULL,
#endif
        .costs = &costs,
        .bus = &bus,
        .tools = app.tools,
        .tools_count = app.tools_count,
        .agent = NULL,
    };

    sc_gateway_config_t gw_config;
    sc_gateway_config_from_cfg(&app.cfg->gateway, &gw_config);
    gw_config.app_ctx = &gw_app_ctx;

    if (with_agent && app.agent_ok && app.agent) {
        gw_app_ctx.agent = app.agent;
        (void)sc_awareness_init(&gw_awareness, &bus);
        if (gw_awareness.bus)
            sc_agent_set_awareness(app.agent, (struct sc_awareness *)&gw_awareness);
        agent_bridge.agent = app.agent;
        agent_bridge.bus = &bus;
        agent_bridge.thread_binding = gw_thread_binding;
        sc_bus_subscribe(&bus, gw_agent_on_message, &agent_bridge, SC_BUS_MESSAGE_RECEIVED);
        fprintf(stderr, "[%s] gateway+agent mode (provider=%s tools=%zu)\n", SC_CODENAME,
                app.cfg->default_provider ? app.cfg->default_provider : "openai", app.tools_count);
    } else {
        fprintf(stderr, "[%s] gateway-only mode (use --with-agent for full agent)\n", SC_CODENAME);
    }

    /* ── Run gateway (blocks) ──────────────────────────────────────────── */
    err = sc_gateway_run(alloc, gw_config.host, gw_config.port, &gw_config);
    if (err != SC_OK)
        fprintf(stderr, "[%s] Gateway error: %s\n", SC_CODENAME, sc_error_string(err));

    /* ── Cleanup: gateway-specific first, then bootstrap ───────────────── */
    if (with_agent && app.agent_ok && app.agent) {
        sc_awareness_deinit(&gw_awareness);
        sc_bus_unsubscribe(&bus, gw_agent_on_message, &agent_bridge);
    }
    if (gw_thread_binding)
        sc_thread_binding_destroy(gw_thread_binding);
    sc_bus_deinit(&bus);
    sc_cost_tracker_deinit(&costs);
    sc_session_manager_deinit(&sessions);
#ifdef SC_HAS_SKILLS
    sc_skillforge_destroy(&skills);
#endif
    sc_app_teardown(&app);
    sc_plugin_unload_all();
    return err;
}

static int run_command(sc_allocator_t *alloc, int argc, char **argv, sc_command_t const *cmd) {
    sc_error_t err = cmd->handler(alloc, argc, argv);
    if (err == SC_OK)
        return 0;
    return 1;
}

static void handle_sighup(int sig) {
    (void)sig;
    sc_config_set_reload_requested();
}

int main(int argc, char *argv[]) {
    sc_allocator_t alloc = sc_system_allocator();

#if defined(__unix__) || defined(__APPLE__)
    signal(SIGHUP, handle_sighup);
#endif

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
