#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "seaclaw/core/error.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/config.h"
#include "seaclaw/agent.h"
#include "seaclaw/agent/cli.h"
#include "seaclaw/provider.h"
#include "seaclaw/channel.h"
#include "seaclaw/tool.h"
#include "seaclaw/memory.h"
#include "seaclaw/security.h"
#include "seaclaw/health.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/onboard.h"
#include "seaclaw/daemon.h"
#include "seaclaw/skillforge.h"
#include "seaclaw/migration.h"
#include "seaclaw/crontab.h"
#include "seaclaw/cli_commands.h"
#include "seaclaw/doctor.h"
#include "seaclaw/gateway.h"

#define SC_VERSION "0.1.0"
#define SC_CODENAME "seaclaw"

typedef struct sc_command {
    const char *name;
    const char *description;
    sc_error_t (*handler)(sc_allocator_t *alloc, int argc, char **argv);
} sc_command_t;

static sc_error_t cmd_agent(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_gateway(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_version(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_help(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_status(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_doctor(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_cron(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_onboard(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_service(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv);
static sc_error_t cmd_migrate(sc_allocator_t *alloc, int argc, char **argv);

static const sc_command_t commands[] = {
    {"agent", "Start interactive agent session", cmd_agent},
    {"gateway", "Start webhook gateway server", cmd_gateway},
    {"service", "Run as background service", cmd_service},
    {"status", "Show runtime status", cmd_status},
    {"onboard", "Interactive setup wizard", cmd_onboard},
    {"doctor", "Run system diagnostics", cmd_doctor},
    {"cron", "Manage scheduled tasks", cmd_cron},
    {"channel", "Channel management", cmd_channel},
    {"skills", "Skill discovery and integration", cmd_skills},
    {"hardware", "Hardware peripheral management", cmd_hardware},
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
    const char *key = sc_config_default_provider_key(&cfg);
    if (key && key[0]) {
        printf("[doctor] provider (%s): ok (API key configured)\n", prov);
    } else {
        printf("[doctor] provider (%s): warning — no API key\n", prov);
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
    (void)alloc;
    (void)argc;
    (void)argv;
    sc_error_t err = sc_daemon_start();
    if (err == SC_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "[%s] daemon not supported on this platform\n", SC_CODENAME);
        return err;
    }
    return err;
}

static sc_error_t cmd_skills(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    sc_skillforge_t sf;
    sc_error_t err = sc_skillforge_create(alloc, &sf);
    if (err != SC_OK) return err;
    const char *dir = (argc >= 3 && argv[2]) ? argv[2] : ".";
    err = sc_skillforge_discover(&sf, dir);
    if (err == SC_OK) {
        sc_skill_t *skills = NULL;
        size_t count = 0;
        sc_skillforge_list_skills(&sf, &skills, &count);
        printf("Skills: %zu discovered\n", count);
        for (size_t i = 0; i < count; i++) {
            printf("  - %s (%s) %s\n", skills[i].name, skills[i].description,
                skills[i].enabled ? "[enabled]" : "[disabled]");
        }
    }
    sc_skillforge_destroy(&sf);
    return err;
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

static sc_error_t cmd_agent(sc_allocator_t *alloc, int argc, char **argv) {
    return sc_agent_cli_run(alloc, (const char *const *)argv, (size_t)argc);
}

static sc_error_t cmd_gateway(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

    sc_gateway_config_t gw_config;
    sc_gateway_config_from_cfg(&cfg.gateway, &gw_config);

    err = sc_gateway_run(alloc, gw_config.host, gw_config.port, &gw_config);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Gateway error: %s\n", SC_CODENAME, sc_error_string(err));
        sc_config_deinit(&cfg);
        return err;
    }
    sc_config_deinit(&cfg);
    return SC_OK;
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
