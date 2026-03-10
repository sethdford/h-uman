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
#include "human/agent.h"
#include "human/agent/awareness.h"
#include "human/agent/cli.h"
#include "human/agent/episodic.h"
#include "human/agent/outcomes.h"
#include "human/agent/spawn.h"
#include "human/bootstrap.h"
#include "human/bus.h"
#include "human/channel.h"
#include "human/channels/thread_binding.h"
#include "human/cli_commands.h"
#include "human/config.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/cost.h"
#include "human/cron.h"
#include "human/crontab.h"
#include "human/daemon.h"
#include "human/doctor.h"
#include "human/gateway.h"
#include "human/gateway/control_protocol.h"
#include "human/health.h"
#include "human/mcp_server.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/factory.h"
#include "human/memory/graph.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/migration.h"
#include "human/observability/log_observer.h"
#include "human/onboard.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/runtime.h"
#include "human/security.h"
#include "human/security/audit.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include "human/session.h"
#include "human/skill_registry.h"
#ifdef HU_HAS_SKILLS
#include "human/skillforge.h"
#endif
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#endif
#ifdef HU_ENABLE_CURL
#include "human/paperclip/client.h"
#include "human/paperclip/heartbeat.h"
#endif
#include "human/plugin_loader.h"
#include "human/tool.h"
#include "human/tools/factory.h"
#if HU_HAS_EMAIL
#include "human/channels/email.h"
#endif
#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"
#endif
#if HU_HAS_IMAP
#include "human/channels/imap.h"
#endif
#if HU_HAS_GMAIL
#include "human/channels/gmail.h"
#endif
#if HU_HAS_TELEGRAM
#include "human/channels/telegram.h"
#endif
#if HU_HAS_DISCORD
#include "human/channels/discord.h"
#endif
#if HU_HAS_SLACK
#include "human/channels/slack.h"
#endif
#if HU_HAS_WHATSAPP
#include "human/channels/whatsapp.h"
#endif
#if HU_HAS_FACEBOOK
#include "human/channels/facebook.h"
#endif
#if HU_HAS_LINE
#include "human/channels/line.h"
#endif
#if HU_HAS_LARK
#include "human/channels/lark.h"
#endif
#if HU_HAS_GOOGLE_CHAT
#include "human/channels/google_chat.h"
#endif
#if HU_HAS_DINGTALK
#include "human/channels/dingtalk.h"
#endif
#if HU_HAS_TEAMS
#include "human/channels/teams.h"
#endif
#if HU_HAS_TWILIO
#include "human/channels/twilio.h"
#endif
#if HU_HAS_ONEBOT
#include "human/channels/onebot.h"
#endif
#if HU_HAS_QQ
#include "human/channels/qq.h"
#endif
#if HU_HAS_INSTAGRAM
#include "human/channels/instagram.h"
#endif
#if HU_HAS_TWITTER
#include "human/channels/twitter.h"
#endif
#if HU_HAS_GOOGLE_RCS
#include "human/channels/google_rcs.h"
#endif
#if HU_HAS_MQTT
#include "human/channels/mqtt.h"
#endif
#if HU_HAS_MATRIX
#include "human/channels/matrix.h"
#endif
#if HU_HAS_IRC
#include "human/channels/irc.h"
#endif
#if HU_HAS_NOSTR
#include "human/channels/nostr.h"
#endif

#define HU_VERSION  "0.3.0"
#define HU_CODENAME "human"

typedef struct hu_command {
    const char *name;
    const char *description;
    hu_error_t (*handler)(hu_allocator_t *alloc, int argc, char **argv);
} hu_command_t;

static hu_error_t cmd_agent(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_gateway(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_mcp(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_version(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_help(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_status(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_doctor(hu_allocator_t *alloc, int argc, char **argv);
#ifdef HU_HAS_CRON
static hu_error_t cmd_cron(hu_allocator_t *alloc, int argc, char **argv);
#endif
static hu_error_t cmd_onboard(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_service(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_service_loop(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_skills(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_migrate(hu_allocator_t *alloc, int argc, char **argv);
#ifdef HU_HAS_PERSONA
static hu_error_t cmd_persona(hu_allocator_t *alloc, int argc, char **argv);
#endif

/* Forward declarations for gateway→agent bridge (used by both service-loop and gateway) */
typedef struct gw_agent_bridge {
    hu_agent_t *agent;
    hu_bus_t *bus;
    hu_thread_binding_t *thread_binding;
} gw_agent_bridge_t;

static bool gw_agent_on_message(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx);

static pthread_mutex_t svc_agent_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool svc_agent_on_message_locked(hu_bus_event_type_t type, const hu_bus_event_t *ev,
                                        void *user_ctx) {
    pthread_mutex_lock(&svc_agent_mutex);
    bool result = gw_agent_on_message(type, ev, user_ctx);
    pthread_mutex_unlock(&svc_agent_mutex);
    return result;
}

#ifdef HU_ENABLE_CURL
static hu_error_t cmd_paperclip(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "heartbeat") == 0)
        return hu_paperclip_heartbeat(alloc, argc - 2, argv + 2);
    fprintf(stderr, "Usage: human paperclip heartbeat\n");
    return HU_ERR_INVALID_ARGUMENT;
}
#endif

static const hu_command_t commands[] = {
    {"agent", "Start interactive agent (--demo: use local Ollama)", cmd_agent},
    {"init", "Initialize config file", cmd_init},
    {"gateway", "Start webhook gateway server", cmd_gateway},
    {"mcp", "Run as MCP server (stdin/stdout JSON-RPC)", cmd_mcp},
    {"service", "Run as background service (daemonize)", cmd_service},
    {"service-loop", "Run service loop in foreground", cmd_service_loop},
    {"status", "Show runtime status", cmd_status},
    {"onboard", "Interactive setup wizard", cmd_onboard},
    {"doctor", "Run system diagnostics", cmd_doctor},
#ifdef HU_HAS_CRON
    {"cron", "Manage scheduled tasks", cmd_cron},
#endif
    {"channel", "Channel management", cmd_channel},
    {"skills", "Skill discovery and integration", cmd_skills},
    {"hardware", "Hardware peripheral management", cmd_hardware},
    {"sandbox", "Show sandbox status and backends", cmd_sandbox},
    {"migrate", "Migrate memory backends", cmd_migrate},
    {"memory", "Memory operations", cmd_memory},
#ifdef HU_HAS_PERSONA
    {"persona", "Create and manage persona profiles", cmd_persona},
#endif
    {"workspace", "Workspace management", cmd_workspace},
    {"capabilities", "Show available capabilities", cmd_capabilities},
    {"models", "List available models", cmd_models},
    {"auth", "Authentication management", cmd_auth},
    {"update", "Check for updates", cmd_update},
#ifdef HU_ENABLE_CURL
    {"paperclip", "Paperclip agent integration", cmd_paperclip},
#endif
    {"version", "Show version information", cmd_version},
    {"help", "Show help information", cmd_help},
};

#define COMMANDS_COUNT (sizeof(commands) / sizeof(commands[0]))

static hu_command_t const *find_command(const char *name) {
    for (size_t i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(commands[i].name, name) == 0)
            return &commands[i];
    }
    return NULL;
}

static void print_usage(FILE *out) {
    fprintf(out, "%s v%s — Autonomous AI Assistant Runtime (C/ASM/WASM)\n\n", HU_CODENAME,
            HU_VERSION);
    fprintf(out, "Usage: human [command] [options]\n\n");
    fprintf(out, "Commands:\n");
    for (size_t i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(commands[i].name, "version") != 0 && strcmp(commands[i].name, "help") != 0)
            fprintf(out, "  %-14s %s\n", commands[i].name, commands[i].description);
    }
    fprintf(out, "  %-14s %s\n", "version", "Show version information");
    fprintf(out, "  %-14s %s\n", "help", "Show help information");
}

static hu_error_t cmd_version(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    printf("%s v%s\n", HU_CODENAME, HU_VERSION);
    return HU_OK;
}

static hu_error_t cmd_help(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    print_usage(stdout);
    return HU_OK;
}

static hu_error_t cmd_status(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    hu_readiness_result_t r = hu_health_check_readiness(alloc);
    printf("Status: %s\n", r.status == HU_READINESS_READY ? "ready" : "not ready");
    if (r.check_count > 0 && r.checks) {
        for (size_t i = 0; i < r.check_count; i++) {
            printf("  %s: %s\n", r.checks[i].name, r.checks[i].healthy ? "ok" : "error");
            if (r.checks[i].message)
                printf("    %s\n", r.checks[i].message);
        }
        alloc->free(alloc->ctx, (void *)r.checks, r.check_count * sizeof(hu_component_check_t));
    } else if (r.check_count == 0) {
        printf("  (no components registered)\n");
    }
    return HU_OK;
}

static hu_error_t cmd_doctor(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        printf("[doctor] config: error — %s\n", hu_error_string(err));
        return err;
    }

    printf("[doctor] config: ok (loaded from %s)\n",
           cfg.config_path && cfg.config_path[0] ? cfg.config_path : "defaults");

    const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
    if (hu_config_provider_requires_api_key(prov)) {
        const char *key = hu_config_default_provider_key(&cfg);
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

    hu_diag_item_t *items = NULL;
    size_t item_count = 0;
    err = hu_doctor_check_config_semantics(alloc, &cfg, &items, &item_count);
    if (err == HU_OK && items && item_count > 0) {
        for (size_t i = 0; i < item_count; i++) {
            const char *sev = items[i].severity == HU_DIAG_ERR    ? "error"
                              : items[i].severity == HU_DIAG_WARN ? "warning"
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
        alloc->free(alloc->ctx, items, item_count * sizeof(hu_diag_item_t));
    }

    hu_config_deinit(&cfg);
    return HU_OK;
}

#ifdef HU_HAS_CRON
static hu_error_t cmd_cron(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "";

    char *path = NULL;
    size_t path_len = 0;
    hu_error_t err = hu_crontab_get_path(alloc, &path, &path_len);
    if (err != HU_OK || !path) {
        fprintf(stderr, "[%s] cron: failed to get crontab path\n", HU_CODENAME);
        return err;
    }
    path[path_len] = '\0';

    if (strcmp(sub, "list") == 0) {
        hu_crontab_entry_t *entries = NULL;
        size_t count = 0;
        err = hu_crontab_load(alloc, path, &entries, &count);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] cron list: %s\n", HU_CODENAME, hu_error_string(err));
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
        hu_crontab_entries_free(alloc, entries, count);
        return HU_OK;
    }

    if (strcmp(sub, "add") == 0) {
        if (argc < 5 || !argv[3] || !argv[4]) {
            alloc->free(alloc->ctx, path, path_len + 1);
            fprintf(stderr, "[%s] cron add <schedule> <command>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
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
            return HU_ERR_OUT_OF_MEMORY;
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
        err = hu_crontab_add(alloc, path, schedule, schedule_len, command, pos, &new_id);
        alloc->free(alloc->ctx, command, cmd_len + 1);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] cron add: %s\n", HU_CODENAME, hu_error_string(err));
            return err;
        }
        printf("Added cron job %s\n", new_id ? new_id : "");
        if (new_id)
            alloc->free(alloc->ctx, new_id, strlen(new_id) + 1);
        return HU_OK;
    }

    if (strcmp(sub, "remove") == 0) {
        if (argc < 4 || !argv[3]) {
            alloc->free(alloc->ctx, path, path_len + 1);
            fprintf(stderr, "[%s] cron remove <id>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        err = hu_crontab_remove(alloc, path, argv[3]);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] cron remove: %s\n", HU_CODENAME, hu_error_string(err));
            return err;
        }
        printf("Removed cron job %s\n", argv[3]);
        return HU_OK;
    }

    alloc->free(alloc->ctx, path, path_len + 1);
    fprintf(stderr, "[%s] cron: use 'list', 'add', or 'remove'\n", HU_CODENAME);
    fprintf(stderr, "  human cron list\n");
    fprintf(stderr, "  human cron add <schedule> <command>\n");
    fprintf(stderr, "  human cron remove <id>\n");
    return HU_ERR_INVALID_ARGUMENT;
}
#endif /* HU_HAS_CRON */

static hu_error_t cmd_onboard(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    return hu_onboard_run(alloc);
}

static hu_error_t cmd_service(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "start";

    if (strcmp(sub, "start") == 0) {
        hu_error_t err = hu_daemon_start();
        if (err == HU_ERR_NOT_SUPPORTED)
            fprintf(stderr, "[%s] daemon not supported on this platform\n", HU_CODENAME);
        else if (err == HU_OK)
            printf("[%s] service started\n", HU_CODENAME);
        else
            fprintf(stderr, "[%s] failed to start service: %s\n", HU_CODENAME,
                    hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "stop") == 0) {
        hu_error_t err = hu_daemon_stop();
        if (err == HU_OK)
            printf("[%s] service stopped\n", HU_CODENAME);
        else if (err == HU_ERR_NOT_FOUND)
            fprintf(stderr, "[%s] service is not running\n", HU_CODENAME);
        else
            fprintf(stderr, "[%s] failed to stop service: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "status") == 0) {
        bool running = hu_daemon_status();
        printf("[%s] service is %s\n", HU_CODENAME, running ? "running" : "stopped");
        return HU_OK;
    }

    if (strcmp(sub, "install") == 0) {
        hu_error_t err = hu_daemon_install(alloc);
        if (err == HU_OK)
            printf("[%s] service installed and started\n", HU_CODENAME);
        else
            fprintf(stderr, "[%s] install failed: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "uninstall") == 0) {
        hu_error_t err = hu_daemon_uninstall();
        if (err == HU_OK)
            printf("[%s] service uninstalled\n", HU_CODENAME);
        else
            fprintf(stderr, "[%s] uninstall failed: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "logs") == 0)
        return hu_daemon_logs();

    fprintf(stderr, "[%s] unknown service subcommand: %s\n", HU_CODENAME, sub);
    fprintf(stderr, "Usage: %s service [start|stop|status|install|uninstall|logs]\n",
            argv[0] ? argv[0] : "human");
    return HU_ERR_INVALID_ARGUMENT;
}

/* Gateway thread context for --with-gateway mode */
typedef struct svc_gw_thread_ctx {
    hu_allocator_t *alloc;
    hu_gateway_config_t config;
    const char *host;
    uint16_t port;
} svc_gw_thread_ctx_t;

static void *svc_gateway_thread(void *arg) {
    svc_gw_thread_ctx_t *ctx = (svc_gw_thread_ctx_t *)arg;
    hu_error_t err = hu_gateway_run(ctx->alloc, ctx->host, ctx->port, &ctx->config);
    if (err != HU_OK)
        fprintf(stderr, "[human] gateway thread error: %s\n", hu_error_string(err));
    return NULL;
}

/* Webhook dispatcher — routes gateway webhook POSTs to the correct channel handler */
typedef struct webhook_dispatcher_ctx {
    hu_allocator_t *alloc;
    hu_service_channel_t *channels;
    size_t ch_count;
} webhook_dispatcher_ctx_t;

static bool webhook_dispatcher(const char *channel, const char *body, size_t body_len, void *ctx) {
    webhook_dispatcher_ctx_t *d = (webhook_dispatcher_ctx_t *)ctx;
    if (!d || !channel || !body)
        return false;
    for (size_t i = 0; i < d->ch_count; i++) {
        const char *name = d->channels[i].channel->vtable->name(d->channels[i].channel_ctx);
        if (!name || strcmp(name, channel) != 0)
            continue;
        if (d->channels[i].webhook_fn) {
            hu_error_t err =
                d->channels[i].webhook_fn(d->channels[i].channel_ctx, d->alloc, body, body_len);
            if (err != HU_OK) {
                (void)fprintf(stderr, "[%s] webhook handler failed (channel=%s): %s\n", HU_CODENAME,
                              channel, hu_error_string(err));
                return false;
            }
        }
        return true;
    }
    return true;
}

static hu_error_t cmd_service_loop(hu_allocator_t *alloc, int argc, char **argv) {
    bool with_gateway = false;
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-gateway") == 0)
            with_gateway = true;
    }

    if (hu_daemon_status()) {
        fprintf(stderr,
                "[%s] ERROR: another service-loop is already running. "
                "Stop it with 'human service stop' before starting a new one.\n",
                HU_CODENAME);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_daemon_write_pid();

    fprintf(stderr, "[%s] service loop started%s\n", HU_CODENAME,
            with_gateway ? " (with gateway)" : "");

    hu_app_ctx_t app_ctx;
    hu_error_t err = hu_app_bootstrap(&app_ctx, alloc, NULL, true, true);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Bootstrap failed: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    hu_bus_t svc_bus;
    hu_bus_init(&svc_bus);
    hu_awareness_t svc_awareness = {0};
    (void)hu_awareness_init(&svc_awareness, &svc_bus);
    if (svc_awareness.bus && app_ctx.agent)
        hu_agent_set_awareness(app_ctx.agent, (struct hu_awareness *)&svc_awareness);

    const char *prov_name =
        app_ctx.cfg->default_provider ? app_ctx.cfg->default_provider : "openai";
    const char *model = app_ctx.cfg->default_model ? app_ctx.cfg->default_model : "";
    fprintf(stderr, "[%s] agent ready (provider=%s model=%s tools=%zu)\n", HU_CODENAME, prov_name,
            model[0] ? model : "(default)", app_ctx.tools_count);

#ifdef HU_HAS_CRON
    fprintf(stderr, "[%s] %zu channel(s) active, cron enabled\n", HU_CODENAME,
            app_ctx.channel_count);

    /* Register proactive engagement cron jobs from persona contacts */
#ifdef HU_HAS_PERSONA
    if (app_ctx.agent && app_ctx.agent->persona && app_ctx.agent->scheduler) {
        const hu_persona_t *persona = app_ctx.agent->persona;
        for (size_t ci = 0; ci < persona->contacts_count; ci++) {
            const hu_contact_profile_t *cp = &persona->contacts[ci];
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
            hu_error_t jerr =
                hu_cron_add_agent_job((hu_cron_scheduler_t *)app_ctx.agent->scheduler, alloc, sched,
                                      prompt, channel_target, job_name, &job_id);
            if (jerr == HU_OK) {
                fprintf(stderr, "[%s] proactive check-in registered for %s (id=%llu sched=%s)\n",
                        HU_CODENAME, cp->name ? cp->name : cp->contact_id,
                        (unsigned long long)job_id, sched);
            }
        }
    }
#endif
#else
    fprintf(stderr, "[%s] %zu channel(s) active\n", HU_CODENAME, app_ctx.channel_count);
#endif

    /* ── Optional gateway on background thread ─────────────────────────── */
    pthread_t gw_tid = 0;
    svc_gw_thread_ctx_t gw_tctx;
    memset(&gw_tctx, 0, sizeof(gw_tctx));
    hu_gateway_config_t gw_config;
    memset(&gw_config, 0, sizeof(gw_config));
    hu_app_context_t svc_app_ctx;
    memset(&svc_app_ctx, 0, sizeof(svc_app_ctx));
    hu_graph_t *svc_graph = NULL;
    gw_agent_bridge_t svc_agent_bridge = {0};
    if (with_gateway) {
        hu_gateway_config_from_cfg(&app_ctx.cfg->gateway, &gw_config);

#ifdef HU_ENABLE_SQLITE
        {
            const char *home = getenv("HOME");
            if (home) {
                char graph_path[1024];
                int np = snprintf(graph_path, sizeof(graph_path), "%s/.human/graph.db", home);
                if (np > 0 && (size_t)np < sizeof(graph_path))
                    (void)hu_graph_open(alloc, graph_path, (size_t)np, &svc_graph);
            }
        }
#endif

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
        hu_bus_subscribe(&svc_bus, svc_agent_on_message_locked, &svc_agent_bridge,
                         HU_BUS_MESSAGE_RECEIVED);

        if (pthread_create(&gw_tid, NULL, svc_gateway_thread, &gw_tctx) == 0) {
            fprintf(stderr, "[%s] gateway listening on %s:%u\n", HU_CODENAME, gw_tctx.host,
                    gw_tctx.port);
        } else {
            fprintf(stderr, "[%s] warning: failed to start gateway thread\n", HU_CODENAME);
            gw_tid = 0;
        }
    }

    /* Initialize behavior thresholds from config */
    hu_conversation_set_thresholds(app_ctx.cfg->behavior.consecutive_limit,
                                   app_ctx.cfg->behavior.participation_pct,
                                   app_ctx.cfg->behavior.max_response_chars,
                                   app_ctx.cfg->behavior.min_response_chars);
    hu_daemon_set_missed_msg_threshold(app_ctx.cfg->behavior.missed_msg_threshold_sec);

    err = hu_service_run(alloc, 1000, app_ctx.channel_count > 0 ? app_ctx.channels : NULL,
                         app_ctx.channel_count, app_ctx.agent, app_ctx.cfg);

    if (with_gateway) {
        hu_bus_unsubscribe(&svc_bus, svc_agent_on_message_locked, &svc_agent_bridge);
        if (gw_tid) {
            pthread_join(gw_tid, NULL);
        }
        if (svc_graph) {
            hu_graph_close(svc_graph, alloc);
            svc_graph = NULL;
        }
    }

    hu_awareness_deinit(&svc_awareness);
    hu_app_teardown(&app_ctx);
    hu_plugin_unload_all();
    hu_daemon_remove_pid();
    return err;
}

#ifdef HU_HAS_SKILLS
static hu_error_t cmd_skills(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "list";

    if (strcmp(sub, "list") == 0) {
        char dir_path[512];
        size_t dlen = hu_skill_registry_get_installed_dir(dir_path, sizeof(dir_path));
        const char *dir = (dlen > 0) ? dir_path : ".";
        hu_skillforge_t sf;
        hu_error_t err = hu_skillforge_create(alloc, &sf);
        if (err != HU_OK)
            return err;
        err = hu_skillforge_discover(&sf, dir);
        if (err == HU_OK) {
            hu_skill_t *skills = NULL;
            size_t count = 0;
            hu_skillforge_list_skills(&sf, &skills, &count);
            printf("Installed skills: %zu\n", count);
            for (size_t i = 0; i < count; i++) {
                printf("  - %s (%s) %s\n", skills[i].name, skills[i].description,
                       skills[i].enabled ? "[enabled]" : "[disabled]");
            }
        }
        hu_skillforge_destroy(&sf);
        return err;
    }

    if (strcmp(sub, "search") == 0) {
        const char *query = (argc >= 4 && argv[3]) ? argv[3] : "";
        hu_skill_registry_entry_t *entries = NULL;
        size_t count = 0;
        hu_error_t err = hu_skill_registry_search(alloc, query, &entries, &count);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] skills search: %s\n", HU_CODENAME, hu_error_string(err));
            return err;
        }
        printf("Registry matches: %zu\n", count);
        for (size_t i = 0; i < count; i++) {
            printf("  - %s v%s — %s\n", entries[i].name,
                   entries[i].version ? entries[i].version : "?",
                   entries[i].description ? entries[i].description : "");
        }
        hu_skill_registry_entries_free(alloc, entries, count);
        return HU_OK;
    }

    if (strcmp(sub, "install") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills install <name>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t err = hu_skill_registry_install(alloc, argv[3]);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] skills install: %s\n", HU_CODENAME, hu_error_string(err));
            return err;
        }
        printf("Installed skill: %s\n", argv[3]);
        return HU_OK;
    }

    if (strcmp(sub, "uninstall") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills uninstall <name>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t err = hu_skill_registry_uninstall(argv[3]);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] skills uninstall: %s\n", HU_CODENAME, hu_error_string(err));
            return err;
        }
        printf("Uninstalled skill: %s\n", argv[3]);
        return HU_OK;
    }

    if (strcmp(sub, "update") == 0) {
        hu_error_t err = hu_skill_registry_update(alloc);
        if (err != HU_OK) {
            fprintf(stderr, "[%s] skills update: %s\n", HU_CODENAME, hu_error_string(err));
            return err;
        }
        printf("Updated all installed skills.\n");
        return HU_OK;
    }

    fprintf(stderr, "[%s] skills: use list, search, install, uninstall, or update\n", HU_CODENAME);
    fprintf(stderr, "  human skills list\n");
    fprintf(stderr, "  human skills search <query>\n");
    fprintf(stderr, "  human skills install <name>\n");
    fprintf(stderr, "  human skills uninstall <name>\n");
    fprintf(stderr, "  human skills update\n");
    fprintf(stderr, "  human skills info <name>\n");
    fprintf(stderr, "  human skills publish [directory]\n");
    return HU_ERR_INVALID_ARGUMENT;
}
#else
static hu_error_t cmd_skills(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    fprintf(stderr, "[%s] skills support not built (compile with HU_ENABLE_SKILLS=ON)\n",
            HU_CODENAME);
    return HU_ERR_NOT_SUPPORTED;
}
#endif

#ifdef HU_HAS_PERSONA
static hu_error_t cmd_persona(hu_allocator_t *alloc, int argc, char **argv) {
    hu_persona_cli_args_t args;
    hu_error_t err = hu_persona_cli_parse(argc, (const char **)argv, &args);
    if (err != HU_OK) {
        fprintf(
            stderr,
            "Usage: human persona <create|update|show|list|delete|validate|export|merge|import> "
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
    return hu_persona_cli_run(alloc, &args);
}
#endif

static hu_error_t cmd_migrate(hu_allocator_t *alloc, int argc, char **argv) {
    hu_migration_config_t mc = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = NULL,
        .source_path_len = 0,
        .target_path = ".",
        .target_path_len = 1,
        .dry_run = true,
    };
    if (argc >= 4 && argv[2] && argv[3]) {
        if (strcmp(argv[2], "sqlite") == 0) {
            mc.source = HU_MIGRATION_SOURCE_SQLITE;
            mc.source_path = argv[3];
            mc.source_path_len = strlen(argv[3]);
        } else if (strcmp(argv[2], "markdown") == 0) {
            mc.source = HU_MIGRATION_SOURCE_MARKDOWN;
            mc.source_path = argv[3];
            mc.source_path_len = strlen(argv[3]);
        }
    }
    if (argc >= 3 && argv[2] && mc.source == HU_MIGRATION_SOURCE_NONE &&
        strcmp(argv[2], "--dry-run") != 0) {
        mc.target_path = argv[2];
        mc.target_path_len = strlen(argv[2]);
    }
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(alloc, &mc, &stats, NULL, NULL);
    if (err == HU_OK) {
        printf("Migration: %zu from sqlite, %zu from markdown, %zu imported\n", stats.from_sqlite,
               stats.from_markdown, stats.imported);
    }
    return err;
}

static hu_error_t cmd_mcp(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;

    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    hu_agent_pool_t *mcp_pool = hu_agent_pool_create(alloc, 8);

    hu_tool_t *tools = NULL;
    size_t tool_count = 0;
    err = hu_tools_create_default(alloc, ".", 1, NULL, &cfg, NULL, NULL, mcp_pool, NULL, &tools,
                                  &tool_count);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Tools init error: %s\n", HU_CODENAME, hu_error_string(err));
        if (mcp_pool)
            hu_agent_pool_destroy(mcp_pool);
        hu_config_deinit(&cfg);
        return err;
    }

    hu_mcp_host_t *srv = NULL;
    err = hu_mcp_host_create(alloc, tools, tool_count, NULL, &srv);
    if (err != HU_OK) {
        hu_tools_destroy_default(alloc, tools, tool_count);
        if (mcp_pool)
            hu_agent_pool_destroy(mcp_pool);
        hu_config_deinit(&cfg);
        return err;
    }

    err = hu_mcp_host_run(srv);

    hu_mcp_host_destroy(srv);
    hu_tools_destroy_default(alloc, tools, tool_count);
    if (mcp_pool)
        hu_agent_pool_destroy(mcp_pool);
    hu_config_deinit(&cfg);
    return err;
}

static hu_error_t cmd_agent(hu_allocator_t *alloc, int argc, char **argv) {
    return hu_agent_cli_run(alloc, (const char *const *)argv, (size_t)argc);
}

/* Bus→agent bridge: streaming context and callback implementation. */
typedef struct gw_stream_ctx {
    hu_bus_t *bus;
    char channel[HU_BUS_CHANNEL_LEN];
    char id[HU_BUS_ID_LEN];
} gw_stream_ctx_t;

static void gw_stream_token_cb(const char *delta, size_t len, void *ctx) {
    gw_stream_ctx_t *sc = (gw_stream_ctx_t *)ctx;
    if (!sc || !delta || !len)
        return;
    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_BUS_MESSAGE_CHUNK;
    memcpy(ev.channel, sc->channel, HU_BUS_CHANNEL_LEN);
    memcpy(ev.id, sc->id, HU_BUS_ID_LEN);
    size_t copy_len = len < HU_BUS_MSG_LEN - 1 ? len : HU_BUS_MSG_LEN - 1;
    memcpy(ev.message, delta, copy_len);
    ev.message[copy_len] = '\0';
    hu_bus_publish(sc->bus, &ev);
}

static bool gw_agent_on_message(hu_bus_event_type_t type, const hu_bus_event_t *ev,
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
    snprintf(stream_ctx.channel, HU_BUS_CHANNEL_LEN, "%s",
             ev->channel[0] ? ev->channel : "gateway");
    snprintf(stream_ctx.id, HU_BUS_ID_LEN, "%s", ev->id);
    hu_error_t err = hu_agent_turn_stream(b->agent, msg, strlen(msg), gw_stream_token_cb,
                                          &stream_ctx, &reply, &reply_len);
    if (err == HU_OK && reply && reply_len > 0) {
        hu_bus_event_t rev;
        memset(&rev, 0, sizeof(rev));
        rev.type = HU_BUS_MESSAGE_SENT;
        int nc = snprintf(rev.channel, HU_BUS_CHANNEL_LEN, "%s",
                          ev->channel[0] ? ev->channel : "gateway");
        if (nc < 0 || (size_t)nc >= HU_BUS_CHANNEL_LEN)
            (void)snprintf(rev.channel, HU_BUS_CHANNEL_LEN, "gateway");
        int ni = snprintf(rev.id, HU_BUS_ID_LEN, "%s", ev->id);
        if (ni < 0 || (size_t)ni >= HU_BUS_ID_LEN)
            rev.id[0] = '\0';
        rev.payload = reply;
        size_t rl = reply_len;
        if (rl >= HU_BUS_MSG_LEN)
            rl = HU_BUS_MSG_LEN - 1;
        memcpy(rev.message, reply, rl);
        rev.message[rl] = '\0';
        hu_bus_publish(b->bus, &rev);
    } else if (err != HU_OK) {
        fprintf(stderr, "[gateway] agent_turn error: %s\n", hu_error_string(err));
        hu_bus_event_t eev;
        memset(&eev, 0, sizeof(eev));
        eev.type = HU_BUS_ERROR;
        int ec = snprintf(eev.channel, HU_BUS_CHANNEL_LEN, "%s",
                          ev->channel[0] ? ev->channel : "gateway");
        if (ec < 0 || (size_t)ec >= HU_BUS_CHANNEL_LEN)
            (void)snprintf(eev.channel, HU_BUS_CHANNEL_LEN, "gateway");
        int ei = snprintf(eev.id, HU_BUS_ID_LEN, "%s", ev->id);
        if (ei < 0 || (size_t)ei >= HU_BUS_ID_LEN)
            eev.id[0] = '\0';
        const char *emsg = hu_error_string(err);
        size_t el = strlen(emsg);
        if (el >= HU_BUS_MSG_LEN)
            el = HU_BUS_MSG_LEN - 1;
        memcpy(eev.message, emsg, el);
        eev.message[el] = '\0';
        hu_bus_publish(b->bus, &eev);
    }
    if (reply)
        b->agent->alloc->free(b->agent->alloc->ctx, reply, reply_len + 1);
    return true;
}

static hu_error_t cmd_gateway(hu_allocator_t *alloc, int argc, char **argv) {
    bool with_agent = false;
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-agent") == 0)
            with_agent = true;
    }

    hu_app_ctx_t app;
    hu_error_t err = hu_app_bootstrap(&app, alloc, NULL, with_agent, false);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Bootstrap failed: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    const char *ws = app.cfg->workspace_dir ? app.cfg->workspace_dir : ".";

    /* ── Gateway-specific subsystems (not in bootstrap) ──────────────────── */
    hu_session_manager_t sessions;
    hu_session_manager_init(&sessions, alloc);

    hu_bus_t bus;
    hu_bus_init(&bus);

    hu_cost_tracker_t costs;
    hu_cost_tracker_init(&costs, alloc, ws, true,
                         app.cfg->cost.daily_limit_usd > 0 ? app.cfg->cost.daily_limit_usd : 100.0,
                         app.cfg->cost.monthly_limit_usd > 0 ? app.cfg->cost.monthly_limit_usd
                                                             : 1000.0,
                         app.cfg->cost.warn_at_percent > 0 ? app.cfg->cost.warn_at_percent : 80);
    hu_cost_load_history(&costs);

#ifdef HU_HAS_SKILLS
    hu_skillforge_t skills;
    memset(&skills, 0, sizeof(skills));
    err = hu_skillforge_create(alloc, &skills);
    if (err != HU_OK)
        fprintf(stderr, "[%s] Skillforge init failed: %s\n", HU_CODENAME, hu_error_string(err));
#endif

    /* Load crontab into bootstrap's cron (bootstrap creates cron but does not load file) */
#ifdef HU_HAS_CRON
    if (app.cron) {
        char *cron_path = NULL;
        size_t cron_path_len = 0;
        if (hu_crontab_get_path(alloc, &cron_path, &cron_path_len) == HU_OK) {
            hu_crontab_entry_t *entries = NULL;
            size_t entry_count = 0;
            if (hu_crontab_load(alloc, cron_path, &entries, &entry_count) == HU_OK) {
                for (size_t ci = 0; ci < entry_count; ci++) {
                    if (entries[ci].enabled && entries[ci].schedule && entries[ci].command) {
                        uint64_t unused_id = 0;
                        hu_cron_add_job((hu_cron_scheduler_t *)app.cron, alloc,
                                        entries[ci].schedule, entries[ci].command, entries[ci].id,
                                        &unused_id);
                    }
                }
                hu_crontab_entries_free(alloc, entries, entry_count);
            }
            alloc->free(alloc->ctx, cron_path, cron_path_len + 1);
        }
    }
#endif

    hu_thread_binding_t *gw_thread_binding =
        with_agent ? hu_thread_binding_create(alloc, 64) : NULL;
    gw_agent_bridge_t agent_bridge = {0};
    hu_awareness_t gw_awareness = {0};

    hu_graph_t *gw_graph = NULL;
#ifdef HU_ENABLE_SQLITE
    {
        const char *home = getenv("HOME");
        if (home) {
            char graph_path[1024];
            int np = snprintf(graph_path, sizeof(graph_path), "%s/.human/graph.db", home);
            if (np > 0 && (size_t)np < sizeof(graph_path))
                (void)hu_graph_open(alloc, graph_path, (size_t)np, &gw_graph);
        }
    }
#endif

    /* ── Gateway app context (hu_app_context_t for RPC handlers) ───────── */
    hu_app_context_t gw_app_ctx = {
        .config = app.cfg,
        .alloc = alloc,
        .sessions = &sessions,
        .cron = app.cron,
#ifdef HU_HAS_SKILLS
        .skills = &skills,
#else
        .skills = NULL,
#endif
        .costs = &costs,
        .bus = &bus,
        .tools = app.tools,
        .tools_count = app.tools_count,
        .agent = NULL,
        .graph = gw_graph,
    };

    hu_gateway_config_t gw_config;
    hu_gateway_config_from_cfg(&app.cfg->gateway, &gw_config);
    gw_config.app_ctx = &gw_app_ctx;

    if (with_agent && app.agent_ok && app.agent) {
        gw_app_ctx.agent = app.agent;
        (void)hu_awareness_init(&gw_awareness, &bus);
        if (gw_awareness.bus)
            hu_agent_set_awareness(app.agent, (struct hu_awareness *)&gw_awareness);
        agent_bridge.agent = app.agent;
        agent_bridge.bus = &bus;
        agent_bridge.thread_binding = gw_thread_binding;
        hu_bus_subscribe(&bus, gw_agent_on_message, &agent_bridge, HU_BUS_MESSAGE_RECEIVED);
        fprintf(stderr, "[%s] gateway+agent mode (provider=%s tools=%zu)\n", HU_CODENAME,
                app.cfg->default_provider ? app.cfg->default_provider : "openai", app.tools_count);
    } else {
        fprintf(stderr, "[%s] gateway-only mode (use --with-agent for full agent)\n", HU_CODENAME);
    }

    /* ── Run gateway (blocks) ──────────────────────────────────────────── */
    err = hu_gateway_run(alloc, gw_config.host, gw_config.port, &gw_config);
    if (err != HU_OK)
        fprintf(stderr, "[%s] Gateway error: %s\n", HU_CODENAME, hu_error_string(err));

    /* ── Cleanup: gateway-specific first, then bootstrap ───────────────── */
    if (with_agent && app.agent_ok && app.agent) {
        hu_awareness_deinit(&gw_awareness);
        hu_bus_unsubscribe(&bus, gw_agent_on_message, &agent_bridge);
    }
    if (gw_graph) {
        hu_graph_close(gw_graph, alloc);
        gw_graph = NULL;
    }
    if (gw_thread_binding)
        hu_thread_binding_destroy(gw_thread_binding);
    hu_bus_deinit(&bus);
    hu_cost_tracker_deinit(&costs);
    hu_session_manager_deinit(&sessions);
#ifdef HU_HAS_SKILLS
    hu_skillforge_destroy(&skills);
#endif
    hu_app_teardown(&app);
    hu_plugin_unload_all();
    return err;
}

static int run_command(hu_allocator_t *alloc, int argc, char **argv, hu_command_t const *cmd) {
    hu_error_t err = cmd->handler(alloc, argc, argv);
    if (err == HU_OK)
        return 0;
    return 1;
}

static void handle_sighup(int sig) {
    (void)sig;
    hu_config_set_reload_requested();
}

int main(int argc, char *argv[]) {
    hu_allocator_t alloc = hu_system_allocator();

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
    hu_command_t const *cmd = find_command(cmd_name);

    if (!cmd) {
        fprintf(stderr, "Unknown command: %s\n", cmd_name);
        fprintf(stderr, "Run 'human help' for usage.\n");
        return 1;
    }

    return run_command(&alloc, argc, argv, cmd) == 0 ? 0 : 1;
}
