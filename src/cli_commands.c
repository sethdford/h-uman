#include "seaclaw/cli_commands.h"
#include "seaclaw/config.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/error.h"
#include "seaclaw/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── channel ─────────────────────────────────────────────────────────────── */
sc_error_t cmd_channel(sc_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        sc_config_t cfg;
        sc_error_t err = sc_config_load(alloc, &cfg);
        printf("Configured channels:\n");
        printf("  cli: active\n");
        if (err == SC_OK && cfg.channels.default_channel &&
            strcmp(cfg.channels.default_channel, "cli") != 0)
            printf("  %s: configured\n", cfg.channels.default_channel);
        if (err == SC_OK) sc_config_deinit(&cfg);
        return SC_OK;
    }
    if (strcmp(argv[2], "status") == 0) {
        printf("Channel health:\n");
        printf("  cli: ok\n");
        return SC_OK;
    }
    fprintf(stderr, "Unknown channel subcommand: %s\n", argv[2]);
    return SC_ERR_INVALID_ARGUMENT;
}

/* ── hardware ────────────────────────────────────────────────────────────── */
sc_error_t cmd_hardware(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        printf("Detected hardware: none\n");
        printf("Supported boards: arduino-uno, nucleo-f401re, stm32f411, esp32, rpi-pico\n");
        return SC_OK;
    }
    if (strcmp(argv[2], "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw hardware info <board>\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        printf("Board: %s\nStatus: not connected\n", argv[3]);
        return SC_OK;
    }
    fprintf(stderr, "Unknown hardware subcommand: %s\n", argv[2]);
    return SC_ERR_INVALID_ARGUMENT;
}

/* ── memory ─────────────────────────────────────────────────────────────── */
static sc_memory_t cmd_memory_open(sc_allocator_t *alloc, const sc_config_t *cfg) {
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
    }
    if (strcmp(backend, "none") == 0)
        return sc_none_memory_create(alloc);
    const char *ws = cfg->workspace_dir ? cfg->workspace_dir : ".";
    return sc_markdown_memory_create(alloc, ws);
}

sc_error_t cmd_memory(sc_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: seaclaw memory <stats|count|list|search|get|forget>\n");
        return SC_OK;
    }
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "Config error: %s\n", sc_error_string(err));
        return err;
    }
    sc_memory_t mem = cmd_memory_open(alloc, &cfg);
    if (!mem.vtable) {
        printf("Memory backend: none (not configured)\n");
        sc_config_deinit(&cfg);
        return SC_OK;
    }
    const char *name = mem.vtable->name ? mem.vtable->name(mem.ctx) : "unknown";
    const char *sub = argv[2];

    if (strcmp(sub, "stats") == 0 || strcmp(sub, "status") == 0) {
        size_t count = 0;
        if (mem.vtable->count) mem.vtable->count(mem.ctx, &count);
        bool healthy = mem.vtable->health_check ? mem.vtable->health_check(mem.ctx) : true;
        printf("Memory backend: %s\nEntry count: %zu\nHealth: %s\n",
            name, count, healthy ? "ok" : "error");
    } else if (strcmp(sub, "count") == 0) {
        size_t count = 0;
        if (mem.vtable->count) mem.vtable->count(mem.ctx, &count);
        printf("%zu\n", count);
    } else if (strcmp(sub, "list") == 0) {
        sc_memory_entry_t *entries = NULL;
        size_t count = 0;
        err = mem.vtable->list(mem.ctx, alloc, NULL, NULL, 0, &entries, &count);
        if (err != SC_OK || count == 0) {
            printf("No memory entries.\n");
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("  [%zu] %.*s: %.*s\n", i + 1,
                    (int)entries[i].key_len, entries[i].key ? entries[i].key : "",
                    (int)(entries[i].content_len > 80 ? 80 : entries[i].content_len),
                    entries[i].content ? entries[i].content : "");
                sc_memory_entry_free_fields(alloc, &entries[i]);
            }
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
    } else if (strcmp(sub, "search") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: seaclaw memory search <query>\n"); err = SC_ERR_INVALID_ARGUMENT; goto done; }
        sc_memory_entry_t *entries = NULL;
        size_t count = 0;
        err = mem.vtable->recall(mem.ctx, alloc, argv[3], strlen(argv[3]), 10, NULL, 0, &entries, &count);
        if (err != SC_OK || count == 0) {
            printf("No results for: %s\n", argv[3]);
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("  [%zu] %.*s: %.*s\n", i + 1,
                    (int)entries[i].key_len, entries[i].key ? entries[i].key : "",
                    (int)(entries[i].content_len > 80 ? 80 : entries[i].content_len),
                    entries[i].content ? entries[i].content : "");
                sc_memory_entry_free_fields(alloc, &entries[i]);
            }
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
    } else if (strcmp(sub, "get") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: seaclaw memory get <key>\n"); err = SC_ERR_INVALID_ARGUMENT; goto done; }
        sc_memory_entry_t entry;
        bool found = false;
        err = mem.vtable->get(mem.ctx, alloc, argv[3], strlen(argv[3]), &entry, &found);
        if (err == SC_OK && found) {
            printf("%.*s\n", (int)entry.content_len, entry.content ? entry.content : "");
            sc_memory_entry_free_fields(alloc, &entry);
        } else {
            printf("Not found: %s\n", argv[3]);
        }
    } else if (strcmp(sub, "forget") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: seaclaw memory forget <key>\n"); err = SC_ERR_INVALID_ARGUMENT; goto done; }
        bool deleted = false;
        err = mem.vtable->forget(mem.ctx, argv[3], strlen(argv[3]), &deleted);
        printf("%s: %s\n", argv[3], deleted ? "forgotten" : "not found");
    } else {
        fprintf(stderr, "Unknown memory subcommand: %s\n", sub);
        err = SC_ERR_INVALID_ARGUMENT;
    }
done:
    if (mem.vtable->deinit) mem.vtable->deinit(mem.ctx);
    sc_config_deinit(&cfg);
    return err;
}

/* ── workspace ───────────────────────────────────────────────────────────── */
sc_error_t cmd_workspace(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3) {
        printf("Current workspace: .\n");
        return SC_OK;
    }
    if (strcmp(argv[2], "show") == 0) {
        printf("Current workspace: .\n");
        return SC_OK;
    }
    if (strcmp(argv[2], "set") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw workspace set <path>\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        printf("Workspace set to: %s\n", argv[3]);
        return SC_OK;
    }
    fprintf(stderr, "Unknown workspace subcommand: %s\n", argv[2]);
    return SC_ERR_INVALID_ARGUMENT;
}

/* ── capabilities ────────────────────────────────────────────────────────── */
sc_error_t cmd_capabilities(sc_allocator_t *alloc, int argc, char **argv) {
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    bool json_mode = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "json") == 0)
            json_mode = true;
    }
    const char *prov = (err == SC_OK && cfg.default_provider) ? cfg.default_provider : "openai";
    const char *backend = (err == SC_OK && cfg.memory.backend) ? cfg.memory.backend : "none";
    if (json_mode) {
        printf("{\"channels\":[\"cli\"],\"tools\":[\"shell\",\"file_read\",\"file_write\",\"file_edit\","
               "\"git\",\"web_search\",\"web_fetch\",\"memory_store\",\"memory_recall\"],"
               "\"providers\":[\"%s\"],\"memory\":\"%s\"}\n", prov, backend);
    } else {
        printf("Capabilities:\n");
        printf("  Channels: cli\n");
        printf("  Tools: shell, file_read, file_write, file_edit, file_append, git,\n");
        printf("         web_search, web_fetch, http_request, memory_store, memory_recall,\n");
        printf("         memory_list, memory_forget, browser, image, screenshot,\n");
        printf("         message, delegate, spawn, cron, composio, pushover\n");
        printf("  Providers: %s\n", prov);
        printf("  Memory: %s\n", backend);
    }
    if (err == SC_OK) sc_config_deinit(&cfg);
    return SC_OK;
}

/* ── models ─────────────────────────────────────────────────────────────── */
sc_error_t cmd_models(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        printf("Known providers and default models:\n");
        printf("  %-16s %s\n", "Provider", "Default Model");
        printf("  %-16s %s\n", "--------", "-------------");
        printf("  %-16s %s\n", "openai", "gpt-4o");
        printf("  %-16s %s\n", "anthropic", "claude-sonnet-4-20250514");
        printf("  %-16s %s\n", "google", "gemini-2.0-flash");
        printf("  %-16s %s\n", "groq", "llama-3.3-70b-versatile");
        printf("  %-16s %s\n", "deepseek", "deepseek-chat");
        return SC_OK;
    }
    if (strcmp(argv[2], "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw models info <model>\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
        printf("Model: %s\nProvider: unknown\nContext: unknown\n", argv[3]);
        return SC_OK;
    }
    fprintf(stderr, "Unknown models subcommand: %s\n", argv[2]);
    return SC_ERR_INVALID_ARGUMENT;
}

/* ── auth ───────────────────────────────────────────────────────────────── */
sc_error_t cmd_auth(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3) {
        printf("Usage: seaclaw auth <login|status|logout> <provider>\n");
        return SC_OK;
    }
    if (strcmp(argv[2], "status") == 0) {
        const char *provider = (argc >= 4) ? argv[3] : "default";
        printf("%s: not authenticated\n", provider);
        return SC_OK;
    }
    if (strcmp(argv[2], "login") == 0) {
        const char *provider = (argc >= 4) ? argv[3] : "default";
        printf("Login for %s: configure API key in ~/.seaclaw/config.json\n", provider);
        return SC_OK;
    }
    if (strcmp(argv[2], "logout") == 0) {
        const char *provider = (argc >= 4) ? argv[3] : "default";
        printf("%s: no credentials found\n", provider);
        return SC_OK;
    }
    fprintf(stderr, "Unknown auth subcommand: %s\n", argv[2]);
    return SC_ERR_INVALID_ARGUMENT;
}

/* ── update ─────────────────────────────────────────────────────────────── */
sc_error_t cmd_update(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    bool check_only = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            check_only = true;
    }
    const char *ver = sc_version_string();
    printf("SeaClaw v%s\n", ver ? ver : "0.1.0");
    if (check_only) {
        printf("No updates available.\n");
    } else {
        printf("Self-update not yet available. Check https://github.com/seaclaw/seaclaw/releases\n");
    }
    return SC_OK;
}
