#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#endif
#include "human/agent.h"
#include "human/agent/awareness.h"
#include "human/agent/cli.h"
#include "human/agent/episodic.h"
#include "human/agent/outcomes.h"
#include "human/agent/registry.h"
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
#include "human/core/log.h"
#include "human/cost.h"
#include "human/cron.h"
#include "human/crontab.h"
#include "human/daemon.h"
#include "human/doctor.h"
#include "human/doctor_fix.h"
#include "human/gateway.h"
#include "human/gateway/control_protocol.h"
#include "human/health.h"
#include "human/mcp_resources.h"
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
#include "human/plugin_discovery.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/runtime.h"
#include "human/security.h"
#include "human/security/audit.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include "human/session.h"
#include "human/skill_registry.h"
#include "human/skill_scaffold.h"
#ifdef HU_HAS_SKILLS
#include "human/skillforge.h"
#endif
#include "human/persona.h"
#ifdef HU_ENABLE_CARTESIA
#include "human/tts/voice_clone.h"
#endif
#ifdef HU_ENABLE_FEEDS
#include "human/feeds/processor.h"
#include "human/feeds/research.h"
#include "human/feeds/trends.h"
#endif
#ifdef HU_ENABLE_ML
#include "human/ml/cli.h"
#endif
#ifdef HU_ENABLE_CURL
#include "human/paperclip/client.h"
#include "human/paperclip/heartbeat.h"
#endif
#ifdef HU_HAS_UPDATE
#include "human/update.h"
#endif
#include "human/plugin_loader.h"
#include "human/pwa.h"
#include "human/pwa_context.h"
#include "human/pwa_learner.h"
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

#define HU_VERSION  "0.5.0"
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
static hu_error_t cmd_plugins(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_agents(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_pwa(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_migrate(hu_allocator_t *alloc, int argc, char **argv);
static hu_error_t cmd_persona(hu_allocator_t *alloc, int argc, char **argv);
#ifdef HU_ENABLE_CARTESIA
static hu_error_t cmd_voice(hu_allocator_t *alloc, int argc, char **argv);
#endif
#ifdef HU_ENABLE_ML
static hu_error_t cmd_ml(hu_allocator_t *alloc, int argc, char **argv);
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

#ifdef HU_ENABLE_ML
static hu_error_t cmd_ml(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "Usage: human ml <subcommand>\n\n"
                "Subcommands:\n"
                "  train                   Train a model from config\n"
                "  experiment              Run experiment loop\n"
                "  prepare                 Tokenize data for training\n"
                "  prepare-conversations   Tokenize chat.db + memory.db for training\n"
                "  dpo-train               Run DPO preference training step\n"
                "  lora-persona            Train LoRA adapter from persona examples\n"
                "  lora-baseline           Score persona example bank fidelity (D2.2)\n"
                "  lora-ab                 Compare pre-/post-LoRA response sets (D2.2)\n"
                "  lora-runner             Generate response set from persona via provider (D2.2)\n"
                "  fidelity-status         Emit JSON status of persona-fidelity health (D2.2)\n"
                "  train-feed-predictor    Train topic/trend predictor from feed data\n"
                "  status                  Show experiment results\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *sub = argv[2];
    if (strcmp(sub, "train") == 0)
        return hu_ml_cli_train(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "experiment") == 0)
        return hu_ml_cli_experiment(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "prepare") == 0)
        return hu_ml_cli_prepare(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "status") == 0)
        return hu_ml_cli_status(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "dpo-train") == 0)
        return hu_ml_cli_dpo_train(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "prepare-conversations") == 0)
        return hu_ml_cli_prepare_conversations(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "lora-persona") == 0)
        return hu_ml_cli_lora_persona(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "lora-baseline") == 0)
        return hu_ml_cli_lora_baseline(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "lora-ab") == 0)
        return hu_ml_cli_lora_ab(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "lora-runner") == 0)
        return hu_ml_cli_lora_runner(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "fidelity-status") == 0)
        return hu_ml_cli_fidelity_status(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "train-feed-predictor") == 0)
        return hu_ml_cli_train_feed_predictor(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "apply-adapter") == 0)
        return hu_ml_cli_apply_adapter(alloc, argc - 2, (const char **)(argv + 2));
    if (strcmp(sub, "--help") == 0 || strcmp(sub, "help") == 0) {
        printf("Usage: human ml <subcommand>\n\n"
               "Subcommands:\n"
               "  train                   Train a model from config\n"
               "  experiment              Run experiment loop\n"
               "  prepare                 Tokenize data for training\n"
               "  prepare-conversations   Tokenize chat.db + memory.db for training\n"
               "  dpo-train               Run DPO preference training step\n"
               "  lora-persona            Train LoRA adapter from persona examples\n"
               "  lora-baseline           Score persona example bank fidelity (D2.2)\n"
               "  lora-ab                 Compare pre-/post-LoRA response sets (D2.2)\n"
               "  lora-runner             Generate response set from persona via provider (D2.2)\n"
               "  fidelity-status         Emit JSON status of persona-fidelity health (D2.2)\n"
               "  apply-adapter           Load a trained adapter into the local provider (W13)\n"
               "  train-feed-predictor    Train topic/trend predictor from feed data\n"
               "  status                  Show experiment results\n");
        return HU_OK;
    }
    fprintf(stderr, "Unknown ml subcommand: %s\n", sub);
    return HU_ERR_INVALID_ARGUMENT;
}
#endif

#ifdef HU_ENABLE_CURL
static hu_error_t cmd_paperclip(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "heartbeat") == 0)
        return hu_paperclip_heartbeat(alloc, argc - 2, argv + 2);
    fprintf(stderr, "Usage: human paperclip heartbeat\n");
    return HU_ERR_INVALID_ARGUMENT;
}
#endif

static hu_error_t cmd_schedule(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME not set\n");
        return HU_ERR_INTERNAL;
    }
    char sched_path[512];
    int sn = snprintf(sched_path, sizeof(sched_path), "%s/.human/scheduled.json", home);
    if (sn < 0 || (size_t)sn >= sizeof(sched_path))
        return HU_ERR_INTERNAL;

    /* Lock the schedule file for the duration of this command */
    char lock_path[520];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", sched_path);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0)
        flock(lock_fd, LOCK_EX);

    if (argc >= 3 && strcmp(argv[2], "list") == 0) {
        hu_error_t lerr = hu_conversation_sched_load(sched_path, (size_t)sn);
        if (lerr != HU_OK && lerr != HU_ERR_IO) {
            fprintf(stderr, "Failed to load schedule: error %d\n", (int)lerr);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            return lerr;
        }
        size_t count = 0;
        /* Print real queue indices so cancel works correctly */
        printf("Scheduled messages:\n");
        for (size_t i = 0; i < HU_SCHED_MAX; i++) {
            hu_sched_slot_t *s = hu_conversation_sched_slot(i);
            if (!s || !s->active)
                continue;
            count++;
            time_t t = (time_t)(s->deliver_at_ms / 1000ULL);
            struct tm tm_buf;
            struct tm *tp = localtime_r(&t, &tm_buf);
            char time_str[64];
            if (tp)
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tp);
            else
                snprintf(time_str, sizeof(time_str), "%" PRIu64, s->deliver_at_ms);
            printf("  [%zu] to: %s | channel: %s | at: %s\n"
                   "       msg: %s\n",
                   i, s->contact_id, s->channel_name[0] ? s->channel_name : "(any)", time_str,
                   s->message);
        }
        if (count == 0)
            printf("  (none)\n");
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return HU_OK;
    }

    if (argc >= 6 && strcmp(argv[2], "add") == 0) {
        const char *contact = argv[3];
        const char *msg = argv[4];
        const char *when_str = argv[5];
        const char *channel = (argc >= 7) ? argv[6] : "";
        uint64_t deliver_ms = 0;
        if (when_str[0] == '+') {
            char *end = NULL;
            long minutes = strtol(when_str + 1, &end, 10);
            if (!end || *end != '\0' || minutes <= 0 || minutes > 525600) {
                fprintf(stderr, "Invalid delay: %s (use +Nm, 1-525600 minutes)\n", when_str);
                if (lock_fd >= 0) {
                    flock(lock_fd, LOCK_UN);
                    close(lock_fd);
                }
                return HU_ERR_INVALID_ARGUMENT;
            }
            deliver_ms = (uint64_t)time(NULL) * 1000ULL + (uint64_t)minutes * 60000ULL;
        } else {
            char *end = NULL;
            long long epoch = strtoll(when_str, &end, 10);
            if (!end || *end != '\0' || epoch <= 0) {
                fprintf(stderr, "Invalid time: %s (use epoch seconds or +Nm)\n", when_str);
                if (lock_fd >= 0) {
                    flock(lock_fd, LOCK_UN);
                    close(lock_fd);
                }
                return HU_ERR_INVALID_ARGUMENT;
            }
            deliver_ms = (uint64_t)epoch * 1000ULL;
        }
        hu_error_t lerr = hu_conversation_sched_load(sched_path, (size_t)sn);
        (void)lerr;
        hu_error_t err = hu_conversation_schedule_message_on(
            contact, strlen(contact), channel, strlen(channel), msg, strlen(msg), deliver_ms);
        if (err != HU_OK) {
            fprintf(stderr, "Failed to schedule message: error %d\n", (int)err);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            return err;
        }
        hu_conversation_sched_save(sched_path, (size_t)sn);
        printf("Scheduled message for %s at %" PRIu64 "ms\n", contact, deliver_ms);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return HU_OK;
    }

    if (argc >= 4 && strcmp(argv[2], "cancel") == 0) {
        char *end = NULL;
        long idx = strtol(argv[3], &end, 10);
        if (!end || *end != '\0' || idx < 0 || idx >= HU_SCHED_MAX) {
            fprintf(stderr, "Invalid index: %s (0-%d)\n", argv[3], HU_SCHED_MAX - 1);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t lerr = hu_conversation_sched_load(sched_path, (size_t)sn);
        (void)lerr;
        hu_sched_slot_t *slot = hu_conversation_sched_slot((size_t)idx);
        if (!slot || !slot->active) {
            fprintf(stderr, "No active scheduled message at index %ld\n", idx);
            if (lock_fd >= 0) {
                flock(lock_fd, LOCK_UN);
                close(lock_fd);
            }
            return HU_ERR_INVALID_ARGUMENT;
        }
        printf("Cancelled: \"%s\" to %s\n", slot->message, slot->contact_id);
        slot->active = false;
        hu_conversation_sched_save(sched_path, (size_t)sn);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }
        return HU_OK;
    }

    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    fprintf(stderr, "Usage: human schedule <subcommand>\n\n"
                    "Subcommands:\n"
                    "  list                           List pending scheduled messages\n"
                    "  add <contact> <msg> <when> [channel]  Schedule a message\n"
                    "    <when>: epoch seconds or +Nm (minutes from now)\n"
                    "  cancel <index>                 Cancel by slot index (shown in list)\n");
    return HU_ERR_INVALID_ARGUMENT;
}

static const hu_command_t commands[] = {
    {"agent", "Start interactive agent (--demo: use local Ollama)", cmd_agent},
    {"init", "Initialize config file", cmd_init},
    {"setup", "Local / on-device model setup", cmd_setup},
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
    {"plugins", "Plugin management", cmd_plugins},
    {"agents", "Manage named agent definitions", cmd_agents},
    {"pwa", "Drive installed PWA web apps", cmd_pwa},
    {"hardware", "Hardware peripheral management", cmd_hardware},
    {"sandbox", "Show sandbox status and backends", cmd_sandbox},
    {"migrate", "Migrate memory backends", cmd_migrate},
    {"memory", "Memory operations", cmd_memory},
    {"persona", "Create and manage persona profiles", cmd_persona},
#ifdef HU_ENABLE_CARTESIA
    {"voice", "Voice cloning and TTS management", cmd_voice},
#endif
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
    {"feed", "Feed monitoring and ingestion", cmd_feed},
#endif
    {"research", "Run research agent", cmd_research},
    {"calibrate", "Analyze messaging patterns and calibrate persona", cmd_calibrate},
    {"workspace", "Workspace management", cmd_workspace},
    {"config", "Configuration reference (schema)", cmd_config},
    {"schedule", "Manage scheduled messages (list, add, cancel)", cmd_schedule},
    {"capabilities", "Show available capabilities", cmd_capabilities},
    {"models", "List available models", cmd_models},
    {"auth", "Authentication management", cmd_auth},
    {"eval", "Run eval suites and compare runs", cmd_eval},
    {"evaluation", "W16 continuous benchmarks (locomo/longmem/dmr/minja/mab/frontier)",
     cmd_evaluation},
    {"hula", "HuLa program engine (parse, compile, execute, trace, replay)", cmd_hula},
    {"update", "Check for updates", cmd_update},
#ifdef HU_ENABLE_CURL
    {"paperclip", "Paperclip agent integration", cmd_paperclip},
#endif
#ifdef HU_ENABLE_ML
    {"ml", "Machine learning training and experiments", cmd_ml},
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
    fprintf(out, "%s v%s — not quite human.\n", HU_CODENAME, HU_VERSION);
    fprintf(out, "Autonomous AI assistant with HuLa program orchestration.\n\n");
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
    /* `human doctor imessage [--json]` — focused channel diagnostics.
     * Always available; non-iMessage builds get a single "not built in" line.
     * --json emits a stable, easy-to-parse object for monitoring integrations
     * (Datadog/Slack alerts/etc.) without scraping pretty-printed text. */
    /* `human doctor verifier [--json]` — focused response-verifier diagnostics.
     * Reads ~/.human/verifier_metrics.json (the daemon flushes once a minute)
     * and reports total runs, claim counts, flagged-rate, and heartbeat
     * freshness. Same JSON format as the imessage subcommand for parity. */
    if (argc >= 3 && argv[2] &&
        (strcmp(argv[2], "imessage") == 0 || strcmp(argv[2], "verifier") == 0 ||
         strcmp(argv[2], "scheduler") == 0 || strcmp(argv[2], "responses") == 0)) {
        bool emit_json = false;
        for (int i = 3; i < argc; i++) {
            if (argv[i] && strcmp(argv[i], "--json") == 0)
                emit_json = true;
        }
        hu_diag_item_t *items = NULL;
        size_t item_count = 0;
        size_t cap = 16;
        items = (hu_diag_item_t *)alloc->alloc(alloc->ctx, sizeof(hu_diag_item_t) * cap);
        if (!items)
            return HU_ERR_OUT_OF_MEMORY;
        int64_t now_epoch = (int64_t)time(NULL);
        /* WARN if no successful poll in the last 10 minutes — well beyond the
         * normal poll interval and short enough to catch silent FDA loss. */
        const int64_t kStaleAfterSecs = 600;
        hu_error_t err = HU_OK;
        if (strcmp(argv[2], "verifier") == 0) {
            /* Verifier flush cadence is 60s; 300s WARN threshold gives 4x
             * headroom and catches a wedged daemon within 5 minutes. WARN at
             * a 10% flagged-rate is a starting point; tune from production. */
            err = hu_doctor_check_verifier(alloc, now_epoch, 300, 0.10, &items, &item_count, &cap);
        } else if (strcmp(argv[2], "imessage") == 0) {
            err = hu_doctor_check_imessage(alloc, now_epoch, kStaleAfterSecs, &items, &item_count,
                                           &cap);
        } else if (strcmp(argv[2], "scheduler") == 0) {
            err = hu_doctor_check_scheduler(alloc, now_epoch, kStaleAfterSecs, &items, &item_count,
                                            &cap);
        } else if (strcmp(argv[2], "responses") == 0) {
            err = hu_doctor_check_response_pipeline(alloc, &items, &item_count, &cap);
        }
        size_t ok_n = 0, warn_n = 0, err_n = 0;
        for (size_t i = 0; i < item_count; i++) {
            if (items[i].severity == HU_DIAG_ERR)
                err_n++;
            else if (items[i].severity == HU_DIAG_WARN)
                warn_n++;
            else
                ok_n++;
        }

        if (emit_json) {
            const char *summary = err_n > 0 ? "error" : (warn_n > 0 ? "warn" : "ok");
            printf("{");
            printf("\"summary\":\"%s\",", summary);
            printf("\"ok\":%zu,\"warn\":%zu,\"error\":%zu,", ok_n, warn_n, err_n);
            printf("\"checked_at_epoch\":%lld,", (long long)now_epoch);
            printf("\"items\":[");
            for (size_t i = 0; i < item_count; i++) {
                const char *sev = (items[i].severity == HU_DIAG_ERR)    ? "error"
                                  : (items[i].severity == HU_DIAG_WARN) ? "warn"
                                                                        : "ok";
                printf("%s{", i == 0 ? "" : ",");
                printf("\"severity\":\"%s\"", sev);
                if (items[i].category) {
                    printf(",\"category\":\"");
                    for (const char *p = items[i].category; *p; p++) {
                        if (*p == '"' || *p == '\\')
                            putchar('\\');
                        putchar(*p);
                    }
                    putchar('"');
                }
                if (items[i].message) {
                    printf(",\"message\":\"");
                    for (const char *p = items[i].message; *p; p++) {
                        if (*p == '"' || *p == '\\')
                            putchar('\\');
                        else if (*p == '\n')
                            printf("\\n");
                        else if ((unsigned char)*p < 0x20)
                            continue;
                        else
                            putchar(*p);
                    }
                    putchar('"');
                }
                putchar('}');
            }
            printf("]}\n");
        } else {
            printf("\n  human doctor %s — diagnostics\n\n", argv[2]);
            for (size_t i = 0; i < item_count; i++) {
                const char *sev_str = (items[i].severity == HU_DIAG_ERR)    ? "error  "
                                      : (items[i].severity == HU_DIAG_WARN) ? "warn   "
                                                                            : "ok     ";
                printf("  %s %s\n", sev_str, items[i].message ? items[i].message : "");
            }
            printf("\n  Summary: %zu ok, %zu warnings, %zu errors\n\n", ok_n, warn_n, err_n);
        }

        for (size_t i = 0; i < item_count; i++) {
            if (items[i].category)
                alloc->free(alloc->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
            if (items[i].message)
                alloc->free(alloc->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
        }
        alloc->free(alloc->ctx, items, cap * sizeof(hu_diag_item_t));
        if (err != HU_OK)
            return err;
        return err_n > 0 ? HU_ERR_INTERNAL : HU_OK;
    }

    bool do_fix = false;
    for (int i = 2; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--fix") == 0)
            do_fix = true;
    }

    if (do_fix) {
        printf("\n  human doctor --fix — auto-repair\n\n");
        hu_doctor_fix_result_t *fixes = NULL;
        size_t fix_count = 0;
        hu_config_t fix_cfg;
        hu_config_load(alloc, &fix_cfg);
        hu_error_t fix_err = hu_doctor_fix(alloc, &fix_cfg, &fixes, &fix_count);
        if (fix_err == HU_OK && fixes) {
            size_t fixed = 0, failed = 0;
            for (size_t i = 0; i < fix_count; i++) {
                printf("  %-20s %s  %s\n", fixes[i].issue ? fixes[i].issue : "?",
                       fixes[i].fixed ? "fixed" : "FAILED",
                       fixes[i].action_taken ? fixes[i].action_taken : "");
                if (fixes[i].fixed)
                    fixed++;
                else
                    failed++;
            }
            printf("\n  %zu fixed, %zu failed\n\n", fixed, failed);
            hu_doctor_fix_results_free(alloc, fixes, fix_count);
        }
        hu_config_deinit(&fix_cfg);
        return HU_OK;
    }

    printf("\n  human doctor — system diagnostics\n\n");

    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        printf("  config        error    %s\n", hu_error_string(err));
        return err;
    }

    printf("  config        ok       loaded from %s\n",
           cfg.config_path && cfg.config_path[0] ? cfg.config_path : "defaults");

    const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
    if (hu_config_provider_requires_api_key(prov)) {
        const char *key = hu_config_default_provider_key(&cfg);
        const char *burl = hu_config_get_provider_base_url(&cfg, prov);
        bool has_vertex_adc =
            burl && strstr(burl, "aiplatform.googleapis.com") != NULL && (!key || !key[0]);
        if (has_vertex_adc) {
            printf("  provider      ok       %s (Vertex AI with ADC)\n", prov);
        } else if (key && key[0]) {
            printf("  provider      ok       %s (API key configured)\n", prov);
        } else {
            printf("  provider      warn     %s — no API key\n", prov);
        }
    } else {
        printf("  provider      ok       %s (local — no API key required)\n", prov);
    }

    const char *backend = cfg.memory_backend ? cfg.memory_backend : "none";
    printf("  memory        %s%s\n", strcmp(backend, "none") == 0 ? "warn     " : "ok       ",
           backend);

    /* Context engine */
    printf("  context       ok       legacy engine (pluggable via hu_context_engine_t)\n");

    hu_diag_item_t *items = NULL;
    size_t item_count = 0;
    err = hu_doctor_check_config_semantics(alloc, &cfg, &items, &item_count);

    size_t ok_count = 0, warn_count = 0, err_count = 0;
    if (err == HU_OK && items && item_count > 0) {
        printf("\n");
        for (size_t i = 0; i < item_count; i++) {
            const char *sev_str;
            if (items[i].severity == HU_DIAG_ERR) {
                sev_str = "error   ";
                err_count++;
            } else if (items[i].severity == HU_DIAG_WARN) {
                sev_str = "warn    ";
                warn_count++;
            } else {
                sev_str = "ok      ";
                ok_count++;
            }
            printf("  %-12s %s %s\n", items[i].category ? items[i].category : "config", sev_str,
                   items[i].message ? items[i].message : "");
        }
    }

    printf("\n  Summary: %zu ok, %zu warnings, %zu errors\n\n", ok_count, warn_count, err_count);

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
        hu_log_error("cron", NULL, "failed to get crontab path");
        return err;
    }
    path[path_len] = '\0';

    if (strcmp(sub, "list") == 0) {
        hu_crontab_entry_t *entries = NULL;
        size_t count = 0;
        err = hu_crontab_load(alloc, path, &entries, &count);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != HU_OK) {
            hu_log_error("cron", NULL, "cron list: %s", hu_error_string(err));
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
            hu_log_error("cron", NULL, "cron add: %s", hu_error_string(err));
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
            hu_log_error("cron", NULL, "cron remove: %s", hu_error_string(err));
            return err;
        }
        printf("Removed cron job %s\n", argv[3]);
        return HU_OK;
    }

    if (strcmp(sub, "add-digest") == 0) {
        if (argc < 4 || !argv[3]) {
            alloc->free(alloc->ctx, path, path_len + 1);
            fprintf(stderr, "[%s] cron add-digest <schedule>\n", HU_CODENAME);
            fprintf(stderr, "  Example: human cron add-digest \"0 9 * * *\"  (daily at 9 AM)\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *schedule = argv[3];
        size_t schedule_len = strlen(schedule);
        const char *command = "human pwa digest";
        size_t cmd_len = strlen(command);
        char *new_id = NULL;
        err = hu_crontab_add(alloc, path, schedule, schedule_len, command, cmd_len, &new_id);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != HU_OK) {
            hu_log_error("cron", NULL, "cron add-digest: %s", hu_error_string(err));
            return err;
        }
        printf("Added PWA digest cron job %s (runs: human pwa digest)\n", new_id ? new_id : "");
        if (new_id)
            alloc->free(alloc->ctx, new_id, strlen(new_id) + 1);
        return HU_OK;
    }

    if (strcmp(sub, "add-learn") == 0) {
        if (argc < 4 || !argv[3]) {
            alloc->free(alloc->ctx, path, path_len + 1);
            fprintf(stderr, "[%s] cron add-learn <schedule>\n", HU_CODENAME);
            fprintf(stderr, "  Example: human cron add-learn \"*/5 * * * *\"  (every 5 min)\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *schedule = argv[3];
        size_t schedule_len = strlen(schedule);
        const char *command = "human pwa learn";
        size_t cmd_len = strlen(command);
        char *new_id = NULL;
        err = hu_crontab_add(alloc, path, schedule, schedule_len, command, cmd_len, &new_id);
        alloc->free(alloc->ctx, path, path_len + 1);
        if (err != HU_OK) {
            hu_log_error("cron", NULL, "cron add-learn: %s", hu_error_string(err));
            return err;
        }
        printf("Added PWA learn cron job %s (runs: human pwa learn)\n", new_id ? new_id : "");
        if (new_id)
            alloc->free(alloc->ctx, new_id, strlen(new_id) + 1);
        return HU_OK;
    }

    alloc->free(alloc->ctx, path, path_len + 1);
    fprintf(stderr, "[%s] cron: use 'list', 'add', 'add-digest', 'add-learn', or 'remove'\n",
            HU_CODENAME);
    fprintf(stderr, "  human cron list\n");
    fprintf(stderr, "  human cron add <schedule> <command>\n");
    fprintf(stderr,
            "  human cron add-digest <schedule>   (schedule PWA digest, e.g. \"0 9 * * *\")\n");
    fprintf(stderr, "  human cron remove <id>\n");
    return HU_ERR_INVALID_ARGUMENT;
}
#endif /* HU_HAS_CRON */

static hu_error_t cmd_onboard(hu_allocator_t *alloc, int argc, char **argv) {
    const char *cli_provider = NULL;
    const char *cli_api_key = NULL;
    bool apple_shortcut = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--apple") == 0) {
            apple_shortcut = true;
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            cli_provider = argv[++i];
        } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
            cli_api_key = argv[++i];
        }
    }

    return hu_onboard_run_with_args(alloc, cli_provider, cli_api_key, apple_shortcut);
}

static hu_error_t cmd_service(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "start";

    if (strcmp(sub, "start") == 0) {
        hu_error_t err = hu_daemon_start();
        if (err == HU_ERR_NOT_SUPPORTED)
            hu_log_error("human", NULL, "daemon not supported on this platform");
        else if (err == HU_OK)
            printf("[%s] service started\n", HU_CODENAME);
        else
            hu_log_error("human", NULL, "failed to start service: %s", hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "stop") == 0) {
        hu_error_t err = hu_daemon_stop();
        if (err == HU_OK)
            printf("[%s] service stopped\n", HU_CODENAME);
        else if (err == HU_ERR_NOT_FOUND)
            hu_log_info("human", NULL, "service is not running");
        else
            hu_log_error("human", NULL, "failed to stop service: %s", hu_error_string(err));
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
            hu_log_error("human", NULL, "install failed: %s", hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "uninstall") == 0) {
        hu_error_t err = hu_daemon_uninstall();
        if (err == HU_OK)
            printf("[%s] service uninstalled\n", HU_CODENAME);
        else
            hu_log_error("human", NULL, "uninstall failed: %s", hu_error_string(err));
        return err;
    }

    if (strcmp(sub, "logs") == 0)
        return hu_daemon_logs();

    hu_log_error("human", NULL, "unknown service subcommand: %s", sub);
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
    for (int attempt = 0; attempt < 5; attempt++) {
        hu_error_t err = hu_gateway_run(ctx->alloc, ctx->host, ctx->port, &ctx->config);
        if (err == HU_OK)
            break;
        hu_log_error("gateway", NULL, "gateway thread error: %s (attempt %d/5)",
                     hu_error_string(err), attempt + 1);
        if (attempt < 4) {
            unsigned int backoff = (unsigned int)(2u << (unsigned)attempt);
            hu_log_info("gateway", NULL, "retrying in %us...", backoff);
            sleep(backoff);
        }
    }
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
                hu_log_error("human", NULL, "webhook handler failed (channel=%s): %s", channel,
                             hu_error_string(err));
                return false;
            }
        }
        return true;
    }
    return true;
}

static hu_error_t cmd_service_loop(hu_allocator_t *alloc, int argc, char **argv) {
    bool with_gateway = false;
    const char *config_path = getenv("HUMAN_CONFIG_PATH");
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-gateway") == 0)
            with_gateway = true;
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
    }

    if (hu_daemon_status()) {
        hu_log_error("human", NULL,
                     "another service-loop is already running. "
                     "Stop it with 'human service stop' before starting a new one.");
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_daemon_write_pid();

    hu_log_info("human", NULL, "service loop started%s", with_gateway ? " (with gateway)" : "");

    hu_app_ctx_t app_ctx;
    hu_error_t err = hu_app_bootstrap(&app_ctx, alloc, config_path, true, true);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "bootstrap failed: %s", hu_error_string(err));
        return err;
    }

    hu_bus_t svc_bus;
    hu_bus_init(&svc_bus);
    hu_awareness_t svc_awareness = {0};
    hu_error_t init_err = hu_awareness_init(&svc_awareness, &svc_bus);
    if (init_err != HU_OK)
        hu_log_error("main", NULL, "awareness init failed: %s", hu_error_string(init_err));
    if (svc_awareness.bus && app_ctx.agent)
        hu_agent_set_awareness(app_ctx.agent, (struct hu_awareness *)&svc_awareness);

    const char *prov_name =
        app_ctx.cfg->default_provider ? app_ctx.cfg->default_provider : "openai";
    const char *model = app_ctx.cfg->default_model ? app_ctx.cfg->default_model : "";
    hu_log_info("human", NULL, "agent ready (provider=%s model=%s tools=%zu)", prov_name,
                model[0] ? model : "(default)", app_ctx.tools_count);

#ifdef HU_HAS_CRON
    hu_log_info("human", NULL, "%zu channel(s) active, cron enabled", app_ctx.channel_count);

    /* Register proactive engagement cron jobs from persona contacts.
     *
     * 2026-05-16 incident follow-up: this cron-based path is INDEPENDENT of
     * hu_service_run_proactive_checkins (which has its own gate at the top).
     * Discovered by local E2E on 2026-05-17 — the daemon was still
     * registering cron jobs for Mindy/Betty/Annie at 10am even though the
     * persona-level master kill switch (proactive.master_enabled) was off.
     * Gate at registration time so cron entries never get created when
     * proactive is disabled. */
    if (app_ctx.agent && app_ctx.agent->persona && app_ctx.agent->scheduler &&
        hu_persona_proactive_is_enabled(app_ctx.agent->persona)) {
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
                hu_log_info(
                    "human", NULL, "proactive check-in registered for %s (id=%llu sched=%s)",
                    cp->name ? cp->name : cp->contact_id, (unsigned long long)job_id, sched);
            }
        }
    }

#ifdef HU_ENABLE_FEEDS
    if (app_ctx.agent && app_ctx.agent->scheduler) {
        const char *rc = hu_research_cron_expression();
        const char *prompt_to_use = hu_research_agent_prompt();
        char *digest = NULL;
        size_t digest_len = 0;
        char *full_prompt = NULL;
        size_t full_len = 0;

#ifdef HU_ENABLE_SQLITE
        sqlite3 *feed_db = hu_sqlite_memory_get_db(app_ctx.memory);
        if (feed_db) {
            int64_t since = (int64_t)time(NULL) - 86400;
            hu_feed_build_daily_digest(alloc, feed_db, since, 4000, &digest, &digest_len);

            char *trend_section = NULL;
            size_t trend_len = 0;
            if (app_ctx.cfg && app_ctx.cfg->feeds.interests) {
                hu_feed_trend_t *trends = NULL;
                size_t trend_count = 0;
                if (hu_feed_detect_trends(alloc, feed_db, app_ctx.cfg->feeds.interests,
                                          strlen(app_ctx.cfg->feeds.interests), &trends,
                                          &trend_count) == HU_OK) {
                    hu_feed_trends_build_section(alloc, trends, trend_count, &trend_section,
                                                 &trend_len);
                    hu_feed_trends_free(alloc, trends, trend_count);
                }
            }

            size_t digest_part = digest ? digest_len : 0;
            size_t trend_part = trend_section ? trend_len : 0;
            size_t combined_len = 0;
            if (trend_part <= SIZE_MAX - digest_part)
                combined_len = digest_part + trend_part;
            if (combined_len > 0) {
                char *combined = (char *)alloc->alloc(alloc->ctx, combined_len + 1);
                if (combined) {
                    size_t p = 0;
                    if (trend_section) {
                        memcpy(combined + p, trend_section, trend_len);
                        p += trend_len;
                    }
                    if (digest) {
                        memcpy(combined + p, digest, digest_len);
                        p += digest_len;
                    }
                    combined[p] = '\0';
                    if (hu_research_build_prompt(alloc, combined, p, &full_prompt, &full_len) ==
                            HU_OK &&
                        full_prompt)
                        prompt_to_use = full_prompt;
                    alloc->free(alloc->ctx, combined, combined_len + 1);
                }
            }

            if (trend_section)
                alloc->free(alloc->ctx, trend_section, trend_len + 1);

            if (app_ctx.cfg->feeds.retention_days > 0) {
                hu_feed_processor_t cleanup_proc = {.alloc = alloc, .db = feed_db};
                hu_feed_processor_cleanup(&cleanup_proc, app_ctx.cfg->feeds.retention_days);
            }
        }
#endif

        uint64_t rid = 0;
        hu_error_t je =
            hu_cron_add_agent_job((hu_cron_scheduler_t *)app_ctx.agent->scheduler, alloc, rc,
                                  prompt_to_use, "cli", "research-agent", &rid);
        if (je == HU_OK)
            hu_log_info("human", NULL, "research agent registered with %s (id=%llu sched=%s)",
                        full_prompt ? "feed digest" : "base prompt", (unsigned long long)rid, rc);

        if (digest)
            alloc->free(alloc->ctx, digest, digest_len + 1);
        if (full_prompt)
            alloc->free(alloc->ctx, full_prompt, full_len + 1);
    }
#endif

#ifdef HU_ENABLE_ML
    /* Weekly DPO preference training — runs every Sunday at 3 AM */
    if (app_ctx.agent && app_ctx.agent->scheduler && app_ctx.agent->sota.sota_initialized) {
        uint64_t dpo_id = 0;
        hu_error_t dpo_err = hu_cron_add_agent_job(
            (hu_cron_scheduler_t *)app_ctx.agent->scheduler, alloc, "0 3 * * 0",
            "Evaluate DPO preference pairs and report alignment score. "
            "Use the dpo-train tool if available, or summarize recent feedback quality.",
            "cli", "dpo-train-weekly", &dpo_id);
        if (dpo_err == HU_OK)
            hu_log_info("human", NULL, "DPO training registered (weekly, id=%llu)",
                        (unsigned long long)dpo_id);

        /* Nightly experiment loop — runs at 2 AM to search for better model configs */
        uint64_t exp_id = 0;
        hu_error_t exp_err = hu_cron_add_agent_job(
            (hu_cron_scheduler_t *)app_ctx.agent->scheduler, alloc, "0 2 * * *",
            "Run ML experiment loop with max 3 iterations to search for better "
            "hyperparameters. Use the ml experiment tool if available. "
            "Report training loss and bits-per-byte improvement.",
            "cli", "ml-experiment-nightly", &exp_id);
        if (exp_err == HU_OK)
            hu_log_info("human", NULL, "ML experiment loop registered (nightly, id=%llu)",
                        (unsigned long long)exp_id);
    }
#endif

#else
    hu_log_info("human", NULL, "%zu channel(s) active", app_ctx.channel_count);
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
                if (np > 0 && (size_t)np < sizeof(graph_path)) {
                    hu_error_t graph_err = hu_graph_open(alloc, graph_path, (size_t)np, &svc_graph);
                    if (graph_err != HU_OK)
                        hu_log_error("main", NULL, "graph open failed: %s",
                                     hu_error_string(graph_err));
                }
            }
        }
#endif

        svc_app_ctx.config = app_ctx.cfg;
        svc_app_ctx.alloc = alloc;
        svc_app_ctx.tools = app_ctx.tools;
        svc_app_ctx.tools_count = app_ctx.tools_count;
        svc_app_ctx.bus = &svc_bus;
        svc_app_ctx.agent = app_ctx.agent;

        static hu_mcp_resource_registry_t svc_mcp_resources;
        hu_mcp_resource_registry_init(&svc_mcp_resources);
        static hu_mcp_prompt_registry_t svc_mcp_prompts;
        hu_mcp_prompt_registry_init(&svc_mcp_prompts);
        for (size_t ti = 0; ti < app_ctx.tools_count && app_ctx.tools; ti++) {
            const hu_tool_t *t = &app_ctx.tools[ti];
            if (!t->vtable || !t->vtable->name)
                continue;
            const char *tn = t->vtable->name(t->ctx);
            const char *td = t->vtable->description ? t->vtable->description(t->ctx) : tn;
            char uri[128];
            snprintf(uri, sizeof(uri), "tool://%s", tn);
            hu_mcp_resource_register(&svc_mcp_resources, uri, tn, td, "application/json");
        }
        svc_app_ctx.mcp_resources = &svc_mcp_resources;
        svc_app_ctx.mcp_prompts = &svc_mcp_prompts;

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
            hu_log_info("human", NULL, "gateway listening on %s:%u", gw_tctx.host, gw_tctx.port);
        } else {
            hu_log_error("human", NULL, "failed to start gateway thread");
            gw_tid = 0;
        }
    }

    /* Initialize behavior thresholds from config */
    hu_conversation_set_thresholds(
        app_ctx.cfg->behavior.consecutive_limit, app_ctx.cfg->behavior.participation_pct,
        app_ctx.cfg->behavior.max_response_chars, app_ctx.cfg->behavior.min_response_chars);
    hu_daemon_set_missed_msg_threshold(app_ctx.cfg->behavior.missed_msg_threshold_sec);

    /* Initialize conversation data (load word lists from embedded JSON) */
    hu_conversation_data_init(alloc);

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
        hu_error_t err = hu_skill_registry_search(alloc, NULL, query, &entries, &count);
        if (err != HU_OK) {
            hu_log_error("skills", NULL, "skills search: %s", hu_error_string(err));
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
            fprintf(stderr, "[%s] skills install <name-or-path>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t err;
        if (strchr(argv[3], '/') || strchr(argv[3], '.'))
            err = hu_skill_registry_install(alloc, argv[3]);
        else
            err = hu_skill_registry_install_by_name(alloc, NULL, argv[3]);
        if (err != HU_OK) {
            hu_log_error("skills", NULL, "skills install: %s", hu_error_string(err));
            return err;
        }
        printf("Installed skill from %s\n", argv[3]);
        return HU_OK;
    }

    if (strcmp(sub, "uninstall") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills uninstall <name>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t err = hu_skill_registry_uninstall(argv[3]);
        if (err != HU_OK) {
            hu_log_error("skills", NULL, "skills uninstall: %s", hu_error_string(err));
            return err;
        }
        printf("Uninstalled skill: %s\n", argv[3]);
        return HU_OK;
    }

    if (strcmp(sub, "update") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills update <path>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_error_t err = hu_skill_registry_update(alloc, argv[3]);
        if (err != HU_OK) {
            hu_log_error("skills", NULL, "skills update: %s", hu_error_string(err));
            return err;
        }
        printf("Updated skill from %s\n", argv[3]);
        return HU_OK;
    }

    if (strcmp(sub, "publish") == 0) {
        const char *dir = (argc >= 4 && argv[3]) ? argv[3] : ".";
        hu_error_t err = hu_skill_registry_publish(alloc, dir);
        if (err != HU_OK) {
            hu_log_error("skills", NULL, "skills publish: %s", hu_error_string(err));
            return err;
        }
        printf("Published skill from %s\n", dir);
        return HU_OK;
    }

    if (strcmp(sub, "info") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "[%s] skills info <name>\n", HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        char dir_path[512];
        size_t dlen = hu_skill_registry_get_installed_dir(dir_path, sizeof(dir_path));
        const char *dir = (dlen > 0) ? dir_path : ".";
        hu_skillforge_t sf;
        hu_error_t err = hu_skillforge_create(alloc, &sf);
        if (err != HU_OK)
            return err;
        err = hu_skillforge_discover(&sf, dir);
        if (err == HU_OK) {
            hu_skill_t *skill = hu_skillforge_get_skill(&sf, argv[3]);
            if (skill) {
                printf("Name:        %s\n", skill->name);
                printf("Description: %s\n", skill->description ? skill->description : "");
                printf("Command:     %s\n", skill->command ? skill->command : "(none)");
                printf("Parameters:  %s\n", skill->parameters ? skill->parameters : "(none)");
                printf("Enabled:     %s\n", skill->enabled ? "yes" : "no");
                if (skill->instructions_path)
                    printf("SKILL.md:    %s\n", skill->instructions_path);
                if (skill->skill_dir)
                    printf("Directory:   %s\n", skill->skill_dir);
                hu_skillforge_destroy(&sf);
                return HU_OK;
            }
        }
        hu_skillforge_destroy(&sf);

        hu_skill_registry_entry_t *entries = NULL;
        size_t count = 0;
        err = hu_skill_registry_search(alloc, NULL, argv[3], &entries, &count);
        if (err == HU_OK && count > 0) {
            for (size_t i = 0; i < count; i++) {
                if (entries[i].name && strcmp(entries[i].name, argv[3]) == 0) {
                    printf("Name:        %s\n", entries[i].name);
                    printf("Description: %s\n",
                           entries[i].description ? entries[i].description : "");
                    printf("Version:     %s\n", entries[i].version ? entries[i].version : "?");
                    printf("Author:      %s\n", entries[i].author ? entries[i].author : "?");
                    printf("URL:         %s\n", entries[i].url ? entries[i].url : "?");
                    printf("Tags:        %s\n", entries[i].tags ? entries[i].tags : "");
                    printf("Status:      not installed\n");
                    hu_skill_registry_entries_free(alloc, entries, count);
                    return HU_OK;
                }
            }
        }
        if (entries)
            hu_skill_registry_entries_free(alloc, entries, count);
        hu_log_error("skills", NULL, "skill '%s' not found", argv[3]);
        return HU_ERR_NOT_FOUND;
    }

    if (strcmp(sub, "init") == 0) {
        const char *name = (argc >= 4 && argv[3]) ? argv[3] : NULL;
        if (!name) {
            fprintf(stderr, "[%s] skills init <name> [--category <cat>] [--author <author>]\n",
                    HU_CODENAME);
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_skill_scaffold_opts_t opts = {
            .name = name,
            .description = NULL,
            .author = NULL,
            .category = HU_SKILL_CATEGORY_GENERAL,
            .output_dir = NULL,
        };
        for (int i = 4; i < argc - 1; i++) {
            if (argv[i] && strcmp(argv[i], "--category") == 0 && argv[i + 1]) {
                if (strcmp(argv[i + 1], "data") == 0)
                    opts.category = HU_SKILL_CATEGORY_DATA;
                else if (strcmp(argv[i + 1], "automation") == 0)
                    opts.category = HU_SKILL_CATEGORY_AUTOMATION;
                else if (strcmp(argv[i + 1], "integration") == 0)
                    opts.category = HU_SKILL_CATEGORY_INTEGRATION;
                else if (strcmp(argv[i + 1], "analysis") == 0)
                    opts.category = HU_SKILL_CATEGORY_ANALYSIS;
                i++;
            } else if (argv[i] && strcmp(argv[i], "--author") == 0 && argv[i + 1]) {
                opts.author = argv[i + 1];
                i++;
            } else if (argv[i] && strcmp(argv[i], "--description") == 0 && argv[i + 1]) {
                opts.description = argv[i + 1];
                i++;
            }
        }
        hu_error_t err = hu_skill_scaffold_init(alloc, &opts);
        if (err != HU_OK) {
            hu_log_error("skills", NULL, "skills init: %s", hu_error_string(err));
            return err;
        }
        printf("Created skill project: %s/\n", name);
        printf("  manifest.json  — skill metadata\n");
        printf("  SKILL.md       — instructions\n");
        printf("\nNext steps:\n");
        printf("  1. Edit manifest.json with your parameters\n");
        printf("  2. Write instructions in SKILL.md\n");
        printf("  3. human skills install %s/\n", name);
        printf("  4. human skills publish %s/\n", name);
        return HU_OK;
    }

    fprintf(stderr,
            "[%s] skills: use list, search, install, info, init, uninstall, update, or publish\n",
            HU_CODENAME);
    fprintf(stderr, "  human skills list\n");
    fprintf(stderr, "  human skills search <query>\n");
    fprintf(stderr, "  human skills install <name-or-path>\n");
    fprintf(stderr, "  human skills uninstall <name>\n");
    fprintf(stderr, "  human skills update <path>\n");
    fprintf(stderr, "  human skills info <name>\n");
    fprintf(stderr, "  human skills init <name>        — scaffold a new skill project\n");
    fprintf(stderr, "  human skills publish [directory]\n");
    fprintf(stderr, "See docs/standards/ai/skills-vs-agents.md (skills vs spawned agents).\n");
    return HU_ERR_INVALID_ARGUMENT;
}
#else
static hu_error_t cmd_skills(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    hu_log_error("skills", NULL, "skills support not built (compile with HU_ENABLE_SKILLS=ON)");
    return HU_ERR_NOT_SUPPORTED;
}
#endif

/* ─── plugins subcommand ─────────────────────────────────────────────────── */

static hu_error_t cmd_plugins(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "list";

    if (strcmp(sub, "list") == 0) {
        hu_plugin_discovery_result_t *results = NULL;
        size_t count = 0;
        hu_error_t err = hu_plugin_discover_and_load(alloc, NULL, NULL, &results, &count);
        if (err != HU_OK) {
            hu_log_error("plugins", NULL, "plugins list: %s", hu_error_string(err));
            return err;
        }
        printf("Discovered plugins: %zu\n", count);
        for (size_t i = 0; i < count; i++) {
            if (results[i].load_error == HU_OK) {
                printf("  + %s v%s  (%s)\n", results[i].name ? results[i].name : "?",
                       results[i].version ? results[i].version : "?",
                       results[i].path ? results[i].path : "?");
            } else {
                printf("  ! %s  FAILED: %s  (%s)\n", results[i].name ? results[i].name : "?",
                       hu_error_string(results[i].load_error),
                       results[i].path ? results[i].path : "?");
            }
        }
        hu_plugin_discovery_results_free(alloc, results, count);
        return HU_OK;
    }

    if (strcmp(sub, "scan") == 0) {
        const char *dir = (argc >= 4 && argv[3]) ? argv[3] : NULL;
        hu_plugin_discovery_result_t *results = NULL;
        size_t count = 0;
        hu_error_t err = hu_plugin_discover_and_load(alloc, dir, NULL, &results, &count);
        if (err != HU_OK) {
            hu_log_error("plugins", NULL, "plugins scan: %s", hu_error_string(err));
            return err;
        }
        printf("Scanned %s: %zu plugins found\n", dir ? dir : "~/.human/plugins/", count);
        for (size_t i = 0; i < count; i++) {
            printf("  %s %s\n", results[i].load_error == HU_OK ? "+" : "!",
                   results[i].name ? results[i].name : results[i].path);
        }
        hu_plugin_discovery_results_free(alloc, results, count);
        return HU_OK;
    }

    if (strcmp(sub, "dir") == 0) {
        char dir[512];
        size_t n = hu_plugin_get_default_dir(dir, sizeof(dir));
        if (n > 0)
            printf("%s\n", dir);
        else
            fprintf(stderr, "Could not determine plugin directory\n");
        return n > 0 ? HU_OK : HU_ERR_NOT_FOUND;
    }

    fprintf(stderr, "[%s] plugins: use list, scan, or dir\n", HU_CODENAME);
    fprintf(stderr, "  human plugins list             — list installed plugins\n");
    fprintf(stderr, "  human plugins scan [directory]  — scan directory for plugins\n");
    fprintf(stderr, "  human plugins dir              — show plugin directory\n");
    return HU_ERR_INVALID_ARGUMENT;
}

/* ─── agents subcommand ──────────────────────────────────────────────────── */

static hu_error_t cmd_agents(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "list";

    char agents_dir[512];
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "HOME not set\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    snprintf(agents_dir, sizeof(agents_dir), "%s/.human/agents", home);

    hu_agent_registry_t reg;
    hu_error_t err = hu_agent_registry_create(alloc, &reg);
    if (err != HU_OK)
        return err;

    err = hu_agent_registry_discover(&reg, agents_dir);
    if (err != HU_OK) {
        hu_agent_registry_destroy(&reg);
        return err;
    }

    if (strcmp(sub, "list") == 0) {
        printf("Registered agents: %zu\n", reg.count);
        for (size_t i = 0; i < reg.count; i++) {
            const hu_named_agent_config_t *a = &reg.agents[i];
            printf("  - %s", a->name ? a->name : "(unnamed)");
            if (a->provider)
                printf(" [%s", a->provider);
            if (a->model)
                printf("/%s", a->model);
            if (a->provider)
                printf("]");
            if (a->role)
                printf(" role=%s", a->role);
            if (a->is_default)
                printf(" (default)");
            printf("\n");
            if (a->description)
                printf("    %s\n", a->description);
        }
        hu_agent_registry_destroy(&reg);
        return HU_OK;
    }

    if (strcmp(sub, "show") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human agents show <name>\n");
            hu_agent_registry_destroy(&reg);
            return HU_ERR_INVALID_ARGUMENT;
        }
        const hu_named_agent_config_t *a = hu_agent_registry_get(&reg, argv[3]);
        if (!a) {
            fprintf(stderr, "Agent '%s' not found\n", argv[3]);
            hu_agent_registry_destroy(&reg);
            return HU_ERR_NOT_FOUND;
        }
        printf("Name:         %s\n", a->name ? a->name : "(none)");
        printf("Provider:     %s\n", a->provider ? a->provider : "(default)");
        printf("Model:        %s\n", a->model ? a->model : "(default)");
        printf("Persona:      %s\n", a->persona ? a->persona : "(none)");
        printf("Role:         %s\n", a->role ? a->role : "(none)");
        printf("Description:  %s\n", a->description ? a->description : "(none)");
        printf("Capabilities: %s\n", a->capabilities ? a->capabilities : "(none)");
        printf("Autonomy:     %u\n", a->autonomy_level);
        printf("Temperature:  %.2f\n", a->temperature);
        printf("Budget:       $%.2f\n", a->budget_usd);
        printf("Max iter:     %u\n", a->max_iterations);
        printf("Default:      %s\n", a->is_default ? "yes" : "no");
        if (a->enabled_tools_count > 0) {
            printf("Tools:        ");
            for (size_t i = 0; i < a->enabled_tools_count; i++) {
                if (i > 0)
                    printf(", ");
                printf("%s", a->enabled_tools[i] ? a->enabled_tools[i] : "?");
            }
            printf("\n");
        }
        if (a->enabled_skills_count > 0) {
            printf("Skills:       ");
            for (size_t i = 0; i < a->enabled_skills_count; i++) {
                if (i > 0)
                    printf(", ");
                printf("%s", a->enabled_skills[i] ? a->enabled_skills[i] : "?");
            }
            printf("\n");
        }
        hu_agent_registry_destroy(&reg);
        return HU_OK;
    }

    hu_agent_registry_destroy(&reg);
    fprintf(stderr, "Usage: human agents [list|show <name>]\n");
    fprintf(
        stderr,
        "Runtime spawn: /spawn or agent_spawn tool; see docs/standards/ai/skills-vs-agents.md\n");
    return HU_ERR_INVALID_ARGUMENT;
}

/* ─── pwa subcommand ─────────────────────────────────────────────────── */

static const char *const PWA_APP_NAMES[] = {"slack",    "discord", "whatsapp", "gmail",
                                            "calendar", "notion",  "twitter",  "telegram",
                                            "linkedin", "facebook"};
#define PWA_APP_COUNT (sizeof(PWA_APP_NAMES) / sizeof(PWA_APP_NAMES[0]))

static const char *pwa_digest_category(const char *app_name) {
    if (!app_name)
        return "App";
    if (strcmp(app_name, "calendar") == 0)
        return "Calendar";
    if (strcmp(app_name, "gmail") == 0)
        return "Email";
    if (strcmp(app_name, "slack") == 0 || strcmp(app_name, "discord") == 0 ||
        strcmp(app_name, "whatsapp") == 0 || strcmp(app_name, "telegram") == 0)
        return "Chat";
    if (strcmp(app_name, "notion") == 0)
        return "Docs";
    if (strcmp(app_name, "twitter") == 0 || strcmp(app_name, "linkedin") == 0 ||
        strcmp(app_name, "facebook") == 0)
        return "Social";
    return "App";
}

static hu_error_t cmd_pwa(hu_allocator_t *alloc, int argc, char **argv) {
    const char *sub = (argc >= 3 && argv[2]) ? argv[2] : "list";

    if (strcmp(sub, "context") == 0) {
#if HU_IS_TEST
        (void)alloc;
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        char *result = NULL;
        size_t result_len = 0;
        hu_error_t err = hu_pwa_context_build(alloc, &result, &result_len);
        if (err != HU_OK) {
            fprintf(stderr, "PWA context: %s\n", hu_error_string(err));
            return err;
        }
        if (result && result_len > 0) {
            printf("%.*s", (int)result_len, result);
            if (result[result_len - 1] != '\n')
                printf("\n");
        } else {
            printf("(no open PWA tabs with content)\n");
        }
        if (result)
            alloc->free(alloc->ctx, result, result_len + 1);
        return HU_OK;
#endif
    }

    if (strcmp(sub, "list") == 0) {
        size_t count = 0;
        (void)hu_pwa_drivers_all(&count);
        printf("PWA Drivers (%zu apps):\n", count);
        for (size_t i = 0; i < PWA_APP_COUNT; i++) {
            const hu_pwa_driver_t *d = hu_pwa_driver_resolve(PWA_APP_NAMES[i]);
            if (!d)
                continue;
            printf("  %-10s %-16s [%s]  %s%s%s\n", d->app_name,
                   d->display_name ? d->display_name : "?", d->url_pattern ? d->url_pattern : "?",
                   d->read_messages_js ? "read " : "", d->send_message_js ? "send " : "",
                   d->navigate_js ? "navigate" : "");
        }
        return HU_OK;
    }

    if (strcmp(sub, "tabs") == 0) {
#if HU_IS_TEST
        (void)alloc;
        (void)argc;
        (void)argv;
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        hu_pwa_browser_t browser;
        hu_error_t err = hu_pwa_detect_browser(&browser);
        if (err != HU_OK) {
            fprintf(stderr,
                    "PWA: no supported browser found (Chrome, Arc, Brave, Edge on macOS)\n");
            return err;
        }
        const char *url_pattern = (argc >= 4 && argv[3]) ? argv[3] : NULL;
        hu_pwa_tab_t *tabs = NULL;
        size_t count = 0;
        err = hu_pwa_list_tabs(alloc, browser, url_pattern, &tabs, &count);
        if (err != HU_OK) {
            fprintf(stderr, "PWA tabs: %s\n", hu_error_string(err));
            return err;
        }
        printf("Browser: %s\n", hu_pwa_browser_name(browser));
        printf("Open tabs (%zu):\n", count);
        for (size_t i = 0; i < count; i++) {
            const hu_pwa_driver_t *drv =
                tabs[i].url ? hu_pwa_driver_find_by_url(tabs[i].url) : NULL;
            printf("  [%d:%d] %s — %s", tabs[i].window_idx, tabs[i].tab_idx,
                   tabs[i].title ? tabs[i].title : "?", tabs[i].url ? tabs[i].url : "?");
            if (drv)
                printf(" [PWA: %s]", drv->app_name);
            printf("\n");
        }
        hu_pwa_tabs_free(alloc, tabs, count);
        return HU_OK;
#endif
    }

    if (strcmp(sub, "read") == 0) {
        if (argc < 4 || !argv[3]) {
            fprintf(stderr, "Usage: human pwa read <app>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *app = argv[3];
#if HU_IS_TEST
        (void)alloc;
        (void)app;
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app);
        if (!drv) {
            fprintf(stderr, "PWA: unknown app '%s'\n", app);
            return HU_ERR_NOT_FOUND;
        }
        if (!drv->read_messages_js) {
            fprintf(stderr, "PWA: app '%s' does not support reading messages\n", app);
            return HU_ERR_NOT_SUPPORTED;
        }
        hu_pwa_browser_t browser;
        hu_error_t err = hu_pwa_detect_browser(&browser);
        if (err != HU_OK) {
            fprintf(stderr, "PWA: no supported browser found\n");
            return err;
        }
        printf("Reading from %s...\n", drv->display_name ? drv->display_name : app);
        fflush(stdout);
        char *result = NULL;
        size_t result_len = 0;
        err = hu_pwa_read_messages(alloc, browser, app, &result, &result_len);
        if (err != HU_OK) {
            fprintf(stderr, "PWA read failed: %s\n", hu_error_string(err));
            return err;
        }
        if (result && result_len > 0) {
            printf("%.*s\n", (int)result_len, result);
            alloc->free(alloc->ctx, result, result_len + 1);
        } else {
            printf("(no messages)\n");
        }
        return HU_OK;
#endif
    }

    if (strcmp(sub, "send") == 0) {
        if (argc < 5 || !argv[3] || !argv[4]) {
            fprintf(stderr, "Usage: human pwa send <app> [target] <message>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *app = argv[3];
        const char *target = NULL;
        const char *message;
        if (argc >= 6 && argv[4] && argv[5]) {
            target = argv[4];
            message = argv[5];
        } else {
            message = argv[4];
        }
#if HU_IS_TEST
        (void)alloc;
        (void)app;
        (void)target;
        (void)message;
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(app);
        if (!drv) {
            fprintf(stderr, "PWA: unknown app '%s'\n", app);
            return HU_ERR_NOT_FOUND;
        }
        if (!drv->send_message_js) {
            fprintf(stderr, "PWA: app '%s' does not support sending messages\n", app);
            return HU_ERR_NOT_SUPPORTED;
        }
        hu_pwa_browser_t browser;
        hu_error_t err = hu_pwa_detect_browser(&browser);
        if (err != HU_OK) {
            fprintf(stderr, "PWA: no supported browser found\n");
            return err;
        }
        char *result = NULL;
        size_t result_len = 0;
        err = hu_pwa_send_message(alloc, browser, app, target, message, &result, &result_len);
        if (err != HU_OK) {
            fprintf(stderr, "PWA send: %s\n", hu_error_string(err));
            return err;
        }
        if (result && result_len > 0) {
            printf("%.*s\n", (int)result_len, result);
            alloc->free(alloc->ctx, result, result_len + 1);
        }
        return HU_OK;
#endif
    }

    if (strcmp(sub, "watch") == 0) {
#if HU_IS_TEST
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        hu_pwa_browser_t browser;
        hu_error_t err = hu_pwa_detect_browser(&browser);
        if (err != HU_OK) {
            fprintf(stderr, "PWA: no supported browser found\n");
            return err;
        }

        int interval = 5;
        if (argc >= 4 && argv[3])
            interval = atoi(argv[3]);
        if (interval < 1)
            interval = 5;

        printf("Watching PWA tabs (poll every %ds, Ctrl-C to stop)...\n", interval);
        printf("Browser: %s\n\n", hu_pwa_browser_name(browser));
        fflush(stdout);

        uint32_t *hashes = (uint32_t *)alloc->alloc(alloc->ctx, PWA_APP_COUNT * sizeof(uint32_t));
        if (!hashes)
            return HU_ERR_OUT_OF_MEMORY;
        memset(hashes, 0, PWA_APP_COUNT * sizeof(uint32_t));

        for (;;) {
            for (size_t i = 0; i < PWA_APP_COUNT; i++) {
                const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(PWA_APP_NAMES[i]);
                if (!drv || !drv->read_messages_js)
                    continue;

                char *result = NULL;
                size_t result_len = 0;
                err = hu_pwa_read_messages(alloc, browser, PWA_APP_NAMES[i], &result, &result_len);
                if (err != HU_OK || !result)
                    continue;

                uint32_t h = 2166136261u;
                for (size_t j = 0; j < result_len; j++)
                    h = (h ^ (uint8_t)result[j]) * 16777619u;

                if (h != hashes[i]) {
                    time_t now = time(NULL);
                    struct tm *tm = localtime(&now);
                    char ts[32];
                    strftime(ts, sizeof(ts), "%H:%M:%S", tm);
                    printf("[%s] %s: new content detected\n", ts,
                           drv->display_name ? drv->display_name : drv->app_name);

                    const char *last = result + result_len;
                    while (last > result && *(last - 1) != '\n')
                        last--;
                    if (*last == '\n')
                        last++;
                    printf("  > %s\n", last);
                    fflush(stdout);
                    hashes[i] = h;
                }
                alloc->free(alloc->ctx, result, result_len + 1);
            }
            sleep((unsigned)interval);
        }
        alloc->free(alloc->ctx, hashes, PWA_APP_COUNT * sizeof(uint32_t));
        return HU_OK;
#endif
    }

    if (strcmp(sub, "digest") == 0) {
#if HU_IS_TEST
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        hu_pwa_browser_t browser;
        hu_error_t err = hu_pwa_detect_browser(&browser);
        if (err != HU_OK) {
            fprintf(stderr, "PWA: no supported browser found\n");
            return err;
        }
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", tm);
        printf("=== PWA Digest (%s) ===\n\n", ts);
        fflush(stdout);

        size_t scanned = 0;
        for (size_t i = 0; i < PWA_APP_COUNT; i++) {
            const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(PWA_APP_NAMES[i]);
            if (!drv || !drv->read_messages_js)
                continue;

            hu_pwa_tab_t tab;
            err = hu_pwa_find_tab(alloc, browser, drv->url_pattern, &tab);
            if (err != HU_OK)
                continue;

            fflush(stdout);
            char *result = NULL;
            size_t result_len = 0;
            err = hu_pwa_exec_js(alloc, &tab, drv->read_messages_js, &result, &result_len);
            hu_pwa_tab_free(alloc, &tab);

            if (err != HU_OK) {
                if (result)
                    alloc->free(alloc->ctx, result, result_len + 1);
                continue;
            }
            const char *label = pwa_digest_category(drv->app_name);
            const char *name = drv->display_name ? drv->display_name : drv->app_name;
            printf("[%s] %s:\n", label, name);
            if (result && result_len > 0) {
                printf("  %.*s\n\n", (int)result_len, result);
                alloc->free(alloc->ctx, result, result_len + 1);
            } else {
                printf("  (no content)\n\n");
                if (result)
                    alloc->free(alloc->ctx, result, result_len + 1);
            }
            scanned++;
        }
        printf("=== End digest (%zu apps scanned) ===\n", scanned);
        return HU_OK;
#endif
    }

    if (strcmp(sub, "scan") == 0) {
#if HU_IS_TEST
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        hu_pwa_browser_t browser;
        hu_error_t err = hu_pwa_detect_browser(&browser);
        if (err != HU_OK) {
            fprintf(stderr, "PWA: no supported browser found\n");
            return err;
        }
        printf("Scanning all open PWA tabs...\n");
        printf("Browser: %s\n\n", hu_pwa_browser_name(browser));
        fflush(stdout);

        size_t found = 0;
        for (size_t i = 0; i < PWA_APP_COUNT; i++) {
            const hu_pwa_driver_t *drv = hu_pwa_driver_resolve(PWA_APP_NAMES[i]);
            if (!drv || !drv->read_messages_js)
                continue;

            hu_pwa_tab_t tab;
            err = hu_pwa_find_tab(alloc, browser, drv->url_pattern, &tab);
            if (err != HU_OK)
                continue;

            printf("=== %s (%s) ===\n", drv->display_name ? drv->display_name : drv->app_name,
                   tab.url ? tab.url : "?");
            fflush(stdout);

            char *result = NULL;
            size_t result_len = 0;
            err = hu_pwa_exec_js(alloc, &tab, drv->read_messages_js, &result, &result_len);
            hu_pwa_tab_free(alloc, &tab);

            if (err == HU_OK && result && result_len > 0) {
                printf("%.*s\n\n", (int)result_len, result);
                alloc->free(alloc->ctx, result, result_len + 1);
                found++;
            } else {
                printf("(no content or read failed)\n\n");
                if (result)
                    alloc->free(alloc->ctx, result, result_len + 1);
            }
        }
        printf("--- Scanned %zu apps with content ---\n", found);
        return HU_OK;
#endif
    }

    if (strcmp(sub, "learn") == 0) {
#if HU_IS_TEST
        fprintf(stderr, "PWA: browser automation unavailable in test build\n");
        return HU_OK;
#else
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "PWA learn: HOME not set\n");
            return HU_ERR_IO;
        }
        char db_path[512];
        int n = snprintf(db_path, sizeof(db_path), "%s/.human/memory.db", home);
        if (n <= 0 || (size_t)n >= sizeof(db_path))
            return HU_ERR_IO;
        hu_memory_t mem = hu_sqlite_memory_create(alloc, db_path);
        if (!mem.vtable) {
            fprintf(stderr, "PWA learn: failed to open memory at %s\n", db_path);
            return HU_ERR_IO;
        }
        hu_pwa_learner_t learner;
        hu_error_t err = hu_pwa_learner_init(alloc, &learner, &mem);
        if (err != HU_OK) {
            fprintf(stderr, "PWA learn: init failed: %s\n", hu_error_string(err));
            mem.vtable->deinit(mem.ctx);
            return err;
        }
        printf("Scanning PWA tabs and storing content in memory...\n");
        fflush(stdout);
        size_t ingested = 0;
        err = hu_pwa_learner_scan(&learner, &ingested);
        if (err != HU_OK)
            fprintf(stderr, "PWA learn: scan error: %s\n", hu_error_string(err));
        else
            printf("Ingested %zu new items into memory (total: %zu)\n", ingested,
                   learner.ingest_count);
        hu_pwa_learner_destroy(&learner);
        mem.vtable->deinit(mem.ctx);
        return err;
#endif
    }

    fprintf(stderr, "Usage: human pwa [list|context|tabs|digest|scan|watch [secs]|learn|read "
                    "<app>|send <app> [target] <message>]\n");
    return HU_ERR_INVALID_ARGUMENT;
}

static hu_error_t cmd_persona(hu_allocator_t *alloc, int argc, char **argv) {
#ifdef HU_HAS_PERSONA
    hu_persona_cli_args_t args;
    hu_error_t err = hu_persona_cli_parse(argc, (const char **)argv, &args);
    if (err != HU_OK) {
        fprintf(stderr, "Usage: human persona "
                        "<create|update|show|list|delete|validate|eval|export|merge|import|filler> "
                        "[name] [options]\n");
        fprintf(
            stderr,
            "  create <name> [--from-imessage] [--from-gmail] [--from-facebook] [--interactive]\n");
        fprintf(stderr, "  update <name> [--from-imessage] [--from-gmail] [--from-facebook]\n");
        fprintf(stderr, "  show <name>\n");
        fprintf(stderr, "  list\n");
        fprintf(stderr, "  delete <name>\n");
        fprintf(stderr, "  validate <name>\n");
        fprintf(stderr, "  eval <name>  (offline persona consistency harness)\n");
        fprintf(stderr, "  export <name>\n");
        fprintf(stderr, "  merge <output_name> <name1> <name2> [name3...]\n");
        fprintf(stderr, "  import <name> [--from-stdin | --from-file <path>]\n");
        fprintf(stderr, "  filler <name> add --channel <name> \"<text>\"     append a filler\n");
        fprintf(stderr, "  filler <name> list --channel <name>             list fillers\n");
        fprintf(stderr, "  filler <name> remove --channel <name> --index N  remove by index\n");
        return err;
    }
    return hu_persona_cli_run(alloc, &args);
#else
    (void)alloc;
    (void)argc;
    (void)argv;
    fprintf(stderr, "Persona support not compiled in (HU_ENABLE_PERSONA=OFF)\n");
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#ifdef HU_ENABLE_CARTESIA
static hu_error_t cmd_voice(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3 || !argv[2]) {
        fprintf(stderr,
                "Usage: human voice <subcommand>\n\n"
                "Subcommands:\n"
                "  clone --file <path> [--name <name>] [--lang <code>] [--persona <name>]\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (strcmp(argv[2], "clone") != 0) {
        fprintf(stderr, "Unknown voice subcommand: %s\n", argv[2]);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *file_path = NULL;
    const char *name = NULL;
    const char *lang = NULL;
    const char *persona = NULL;

    for (int i = 3; i < argc; i++) {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file_path = argv[++i];
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            name = argv[++i];
        else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc)
            lang = argv[++i];
        else if (strcmp(argv[i], "--persona") == 0 && i + 1 < argc)
            persona = argv[++i];
    }

    if (!file_path) {
        fprintf(stderr, "Error: --file <path> is required\n");
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Get Cartesia API key from env, then config fallback */
    const char *api_key = getenv("CARTESIA_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        hu_config_t tmp_cfg;
        if (hu_config_load(alloc, &tmp_cfg) == HU_OK) {
            const char *cfg_key = hu_config_get_provider_key(&tmp_cfg, "cartesia");
            if (cfg_key && cfg_key[0] != '\0')
                api_key = cfg_key;
        }
    }
    if (!api_key || api_key[0] == '\0') {
        fprintf(stderr, "Error: set CARTESIA_API_KEY or configure providers.cartesia.api_key\n");
        return HU_ERR_PROVIDER_AUTH;
    }

    hu_voice_clone_config_t cfg;
    hu_voice_clone_config_default(&cfg);
    if (name) {
        cfg.name = name;
        cfg.name_len = strlen(name);
    }
    if (lang) {
        cfg.language = lang;
        cfg.language_len = strlen(lang);
    }

    hu_voice_clone_result_t result;
    hu_error_t err =
        hu_voice_clone_from_file(alloc, api_key, strlen(api_key), file_path, &cfg, &result);
    if (err != HU_OK) {
        if (result.error_len > 0)
            fprintf(stderr, "Clone failed: %s\n", result.error);
        else
            fprintf(stderr, "Clone failed: %s\n", hu_error_string(err));
        return err;
    }

    printf("Voice cloned successfully!\n");
    printf("  Voice ID: %s\n", result.voice_id);
    printf("  Name:     %s\n", result.name);
    printf("  Language:  %s\n", result.language);

    if (persona && persona[0]) {
        err = hu_persona_set_voice_id(alloc, persona, strlen(persona), result.voice_id,
                                      strlen(result.voice_id));
        if (err == HU_OK)
            printf("\nPersona '%s' updated with new voice_id.\n", persona);
        else
            fprintf(stderr, "\nWarning: failed to update persona '%s': %s\n", persona,
                    hu_error_string(err));
    } else {
        printf("\nTo use this voice, add it to your persona:\n");
        printf("  human voice clone --file %s --persona <name>\n", file_path);
        printf("Or manually set voice_id in ~/.human/personas/<name>.json\n");
    }

    return HU_OK;
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
        hu_log_error("human", NULL, "config error: %s", hu_error_string(err));
        return err;
    }

    hu_agent_pool_t *mcp_pool = hu_agent_pool_create(alloc, 8);

    hu_tool_t *tools = NULL;
    size_t tool_count = 0;
    err = hu_tools_create_default(alloc, ".", 1, NULL, &cfg, NULL, NULL, mcp_pool, NULL, NULL, NULL,
                                  &tools, &tool_count);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "tools init error: %s", hu_error_string(err));
        if (mcp_pool)
            hu_agent_pool_destroy(mcp_pool);
        hu_config_deinit(&cfg);
        return err;
    }

    hu_mcp_resource_registry_t mcp_res;
    hu_mcp_resource_registry_init(&mcp_res);
    hu_mcp_prompt_registry_t mcp_pr;
    hu_mcp_prompt_registry_init(&mcp_pr);
    for (size_t i = 0; i < tool_count; i++) {
        if (!tools[i].vtable || !tools[i].vtable->name)
            continue;
        const char *tn = tools[i].vtable->name(tools[i].ctx);
        const char *td =
            tools[i].vtable->description ? tools[i].vtable->description(tools[i].ctx) : tn;
        char uri[128];
        snprintf(uri, sizeof(uri), "tool://%s", tn);
        hu_mcp_resource_register(&mcp_res, uri, tn, td, "application/json");
    }

    hu_mcp_engine_t *srv = NULL;
    err = hu_mcp_engine_create(alloc, tools, tool_count, NULL, &srv);
    if (err != HU_OK) {
        hu_tools_destroy_default(alloc, tools, tool_count);
        if (mcp_pool)
            hu_agent_pool_destroy(mcp_pool);
        hu_config_deinit(&cfg);
        return err;
    }
    hu_mcp_engine_set_resources(srv, &mcp_res);
    hu_mcp_engine_set_prompts(srv, &mcp_pr);

    err = hu_mcp_engine_run(srv);

    hu_mcp_engine_destroy(srv);
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

/* Back up to last valid UTF-8 boundary at or before pos in buf of length len. */
static size_t utf8_safe_truncate(const char *buf, size_t len) {
    if (len == 0)
        return 0;
    size_t pos = len;
    while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80)
        --pos;
    if (pos > 0) {
        unsigned char lead = (unsigned char)buf[pos - 1];
        size_t seq_len = 1;
        if ((lead & 0xE0) == 0xC0)
            seq_len = 2;
        else if ((lead & 0xF0) == 0xE0)
            seq_len = 3;
        else if ((lead & 0xF8) == 0xF0)
            seq_len = 4;
        if (pos - 1 + seq_len > len)
            pos = pos - 1;
    }
    return pos;
}

/* Copy data into bus event message with UTF-8 safe truncation */
static void gw_bus_set_message(hu_bus_event_t *bev, const char *data, size_t len) {
    if (!data || len == 0) {
        bev->message[0] = '\0';
        return;
    }
    size_t copy_len = len < HU_BUS_MSG_LEN - 1 ? len : HU_BUS_MSG_LEN - 1;
    if (copy_len < len)
        copy_len = utf8_safe_truncate(data, copy_len);
    memcpy(bev->message, data, copy_len);
    bev->message[copy_len] = '\0';
}

/* Rich stream event callback for v2: maps agent events to typed bus events */
static void gw_stream_event_cb(const hu_agent_stream_event_t *event, void *ctx) {
    gw_stream_ctx_t *sc = (gw_stream_ctx_t *)ctx;
    if (!sc)
        return;
    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.channel, sc->channel, HU_BUS_CHANNEL_LEN);
    memcpy(ev.id, sc->id, HU_BUS_ID_LEN);

    switch (event->type) {
    case HU_AGENT_STREAM_TEXT:
        if (!event->data || event->data_len == 0)
            return;
        ev.type = HU_BUS_MESSAGE_CHUNK;
        gw_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_THINKING:
        if (!event->data || event->data_len == 0)
            return;
        ev.type = HU_BUS_THINKING_CHUNK;
        gw_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_TOOL_START:
        ev.type = HU_BUS_TOOL_CALL;
        gw_bus_set_message(&ev, event->tool_name, event->tool_name_len);
        break;
    case HU_AGENT_STREAM_TOOL_ARGS:
        ev.type = HU_BUS_TOOL_CALL;
        gw_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_TOOL_RESULT:
        ev.type = HU_BUS_TOOL_CALL_RESULT;
        gw_bus_set_message(&ev, event->data, event->data_len);
        break;
    }
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
    hu_error_t err = hu_agent_turn_stream_v2(b->agent, msg, strlen(msg), gw_stream_event_cb,
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
        hu_log_error("gateway", NULL, "agent_turn error: %s", hu_error_string(err));
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
    const char *config_path = getenv("HUMAN_CONFIG_PATH");
    for (int i = 2; i < argc && argv[i]; i++) {
        if (strcmp(argv[i], "--with-agent") == 0)
            with_agent = true;
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
    }

    hu_app_ctx_t app;
    hu_error_t err = hu_app_bootstrap(&app, alloc, config_path, with_agent, false);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "bootstrap failed: %s", hu_error_string(err));
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
        hu_log_error("human", NULL, "skillforge init failed: %s", hu_error_string(err));
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
            if (np > 0 && (size_t)np < sizeof(graph_path)) {
                hu_error_t graph_err = hu_graph_open(alloc, graph_path, (size_t)np, &gw_graph);
                if (graph_err != HU_OK)
                    hu_log_error("main", NULL, "graph open failed: %s", hu_error_string(graph_err));
            }
        }
    }
#endif

    /* ── MCP resource + prompt registries ─────────────────────────────── */
    hu_mcp_resource_registry_t mcp_resources;
    hu_mcp_resource_registry_init(&mcp_resources);
    hu_mcp_prompt_registry_t mcp_prompts;
    hu_mcp_prompt_registry_init(&mcp_prompts);

    for (size_t ti = 0; ti < app.tools_count && app.tools; ti++) {
        const hu_tool_t *t = &app.tools[ti];
        if (!t->vtable || !t->vtable->name)
            continue;
        const char *tname = t->vtable->name(t->ctx);
        const char *tdesc = t->vtable->description ? t->vtable->description(t->ctx) : tname;
        char uri[128];
        snprintf(uri, sizeof(uri), "tool://%s", tname);
        hu_mcp_resource_register(&mcp_resources, uri, tname, tdesc, "application/json");
    }

    {
        hu_mcp_prompt_t p = {0};
        snprintf(p.name, sizeof(p.name), "system");
        snprintf(p.description, sizeof(p.description), "Default system prompt for the agent");
        snprintf(p.template_text, sizeof(p.template_text), "You are a helpful assistant.");
        p.template_text_len = strlen(p.template_text);
        hu_mcp_prompt_register(&mcp_prompts, &p);
    }
    {
        hu_mcp_prompt_t p = {0};
        snprintf(p.name, sizeof(p.name), "persona");
        snprintf(p.description, sizeof(p.description), "Apply a persona profile to the agent");
        p.argument_count = 1;
        snprintf(p.arguments[0].name, sizeof(p.arguments[0].name), "name");
        snprintf(p.arguments[0].description, sizeof(p.arguments[0].description), "Persona name");
        p.arguments[0].required = true;
        snprintf(p.template_text, sizeof(p.template_text), "Adopt the persona: {name}");
        p.template_text_len = strlen(p.template_text);
        hu_mcp_prompt_register(&mcp_prompts, &p);
    }

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
        .mcp_resources = &mcp_resources,
        .mcp_prompts = &mcp_prompts,
    };

    hu_gateway_config_t gw_config;
    hu_gateway_config_from_cfg(&app.cfg->gateway, &gw_config);
    gw_config.app_ctx = &gw_app_ctx;

    if (with_agent && app.agent_ok && app.agent) {
        gw_app_ctx.agent = app.agent;
        hu_error_t init_err = hu_awareness_init(&gw_awareness, &bus);
        if (init_err != HU_OK)
            hu_log_error("main", NULL, "awareness init failed: %s", hu_error_string(init_err));
        if (gw_awareness.bus)
            hu_agent_set_awareness(app.agent, (struct hu_awareness *)&gw_awareness);
        agent_bridge.agent = app.agent;
        agent_bridge.bus = &bus;
        agent_bridge.thread_binding = gw_thread_binding;
        hu_bus_subscribe(&bus, gw_agent_on_message, &agent_bridge, HU_BUS_MESSAGE_RECEIVED);
        hu_log_info("human", NULL, "gateway+agent mode (provider=%s tools=%zu)",
                    app.cfg->default_provider ? app.cfg->default_provider : "openai",
                    app.tools_count);
        if (gw_graph && app.agent && app.agent->retrieval_engine)
            hu_retrieval_set_graph(app.agent->retrieval_engine, gw_graph);
    } else {
        hu_log_info("human", NULL, "gateway-only mode (use --with-agent for full agent)");
    }

    /* ── Run gateway (blocks) ──────────────────────────────────────────── */
    err = hu_gateway_run(alloc, gw_config.host, gw_config.port, &gw_config);
    if (err != HU_OK)
        hu_log_error("gateway", NULL, "gateway error: %s", hu_error_string(err));

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
    hu_conversation_data_cleanup();
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

/* Load KEY=VALUE pairs from a .env file into the process environment.
 * Skips comments (#), empty lines, and lines without '='.
 * Does NOT override existing env vars. */
static void load_dotenv(const char *path) {
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* Trim trailing whitespace from key */
        char *ke = eq - 1;
        while (ke > key && (*ke == ' ' || *ke == '\t'))
            *ke-- = '\0';
        /* Trim newline/quotes from value */
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r'))
            val[--vlen] = '\0';
        /* Strip surrounding quotes */
        if (vlen >= 2 && ((val[0] == '"' && val[vlen - 1] == '"') ||
                          (val[0] == '\'' && val[vlen - 1] == '\''))) {
            val[vlen - 1] = '\0';
            val++;
        }
        if (key[0] && !getenv(key))
            setenv(key, val, 0);
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Load .env files: project-local, ~/.human/.env */
    load_dotenv(".env");
    {
        const char *home = getenv("HOME");
        if (home) {
            char envpath[512];
            snprintf(envpath, sizeof(envpath), "%s/.human/.env", home);
            load_dotenv(envpath);
        }
    }

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

#ifndef HU_IS_TEST
    if (hu_onboard_check_first_run() && strcmp(cmd_name, "onboard") != 0 &&
        strcmp(cmd_name, "init") != 0 && strcmp(cmd_name, "help") != 0 &&
        strcmp(cmd_name, "version") != 0) {
        fprintf(stderr, "No config found. Run 'human onboard' to set up.\n\n");
    }
#endif

#if defined(HU_HAS_UPDATE) && !HU_IS_TEST
    if (strcmp(cmd_name, "update") != 0 && strcmp(cmd_name, "version") != 0 &&
        strcmp(cmd_name, "help") != 0) {
        hu_config_t update_cfg;
        if (hu_config_load(&alloc, &update_cfg) == HU_OK) {
            hu_update_maybe_check(&alloc, &update_cfg);
            hu_config_deinit(&update_cfg);
        }
    }
#endif

    return run_command(&alloc, argc, argv, cmd) == 0 ? 0 : 1;
}
