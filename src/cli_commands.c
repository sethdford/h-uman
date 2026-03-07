#include "seaclaw/cli_commands.h"
#include "seaclaw/config.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/factory.h"
#include "seaclaw/security/sandbox.h"
#ifdef SC_HAS_UPDATE
#include "seaclaw/update.h"
#endif
#include "seaclaw/version.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SC_INIT_CONFIG_DIR  ".seaclaw"
#define SC_INIT_CONFIG_FILE "config.json"
#define SC_INIT_MAX_PATH    1024

#ifndef SC_IS_TEST
static const char SC_INIT_DEFAULT_JSON[] =
    "{\n"
    "  \"default_provider\": \"gemini\",\n"
    "  \"default_model\": \"gemini-3.1-flash-lite-preview\",\n"
    "  \"max_tokens\": 4096,\n"
    "  \"memory\": {\n"
    "    \"backend\": \"sqlite\"\n"
    "  },\n"
    "  \"gateway\": {\n"
    "    \"enabled\": false,\n"
    "    \"port\": 3000\n"
    "  }\n"
    "}\n";
#endif

/* ── init ────────────────────────────────────────────────────────────────── */
sc_error_t cmd_init(sc_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;

#ifdef SC_IS_TEST
    /* In test mode: skip filesystem and stdin, succeed immediately. */
    return SC_OK;
#else
    const char *home = getenv("HOME");
    if (!home)
        home = ".";

    char config_path[SC_INIT_MAX_PATH];
    int n = snprintf(config_path, sizeof(config_path), "%s/%s/%s", home, SC_INIT_CONFIG_DIR,
                     SC_INIT_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= sizeof(config_path))
        return SC_ERR_INVALID_ARGUMENT;

    if (access(config_path, F_OK) == 0) {
        printf("Config already exists. Overwrite? [y/N] ");
        fflush(stdout);
        int c = getchar();
        if (c != 'y' && c != 'Y') {
            printf("Aborted.\n");
            return SC_ERR_CANCELLED;
        }
        while (c != '\n' && c != EOF)
            c = getchar();
    }

    char dir_path[SC_INIT_MAX_PATH];
    n = snprintf(dir_path, sizeof(dir_path), "%s/%s", home, SC_INIT_CONFIG_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir_path))
        return SC_ERR_INVALID_ARGUMENT;

    if (mkdir(dir_path, 0700) != 0 && errno != EEXIST)
        return SC_ERR_IO;

    FILE *f = fopen(config_path, "w");
    if (!f)
        return SC_ERR_IO;

    size_t len = sizeof(SC_INIT_DEFAULT_JSON) - 1;
    if (fwrite(SC_INIT_DEFAULT_JSON, 1, len, f) != len) {
        fclose(f);
        return SC_ERR_IO;
    }
    fclose(f);

    printf("Created ~/.seaclaw/config.json\n");
    printf("Set your API key: export OPENAI_API_KEY=sk-...\n");
    printf("Start chatting: seaclaw agent\n");
    return SC_OK;
#endif /* !SC_IS_TEST */
}

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
        if (err == SC_OK)
            sc_config_deinit(&cfg);
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
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);

    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        if (err == SC_OK) {
            printf("Peripherals: %s\n", cfg.peripherals.enabled ? "enabled" : "disabled");
            if (cfg.hardware.enabled) {
                printf("Hardware transport: %s\n",
                       cfg.hardware.transport ? cfg.hardware.transport : "auto");
                if (cfg.hardware.serial_port)
                    printf("Serial port: %s @ %u baud\n", cfg.hardware.serial_port,
                           cfg.hardware.baud_rate);
                if (cfg.hardware.probe_target)
                    printf("Probe target: %s\n", cfg.hardware.probe_target);
            } else {
                printf("Detected hardware: none\n");
            }
        } else {
            printf("Detected hardware: none\n");
        }
        printf("Supported boards: arduino-uno, nucleo-f401re, stm32f411, esp32, rpi-pico\n");
        if (err == SC_OK)
            sc_config_deinit(&cfg);
        return SC_OK;
    }
    if (strcmp(argv[2], "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw hardware info <board>\n");
            if (err == SC_OK)
                sc_config_deinit(&cfg);
            return SC_ERR_INVALID_ARGUMENT;
        }
        printf("Board: %s\nStatus: not connected\n", argv[3]);
        if (err == SC_OK)
            sc_config_deinit(&cfg);
        return SC_OK;
    }
    if (err == SC_OK)
        sc_config_deinit(&cfg);
    fprintf(stderr, "Unknown hardware subcommand: %s\n", argv[2]);
    return SC_ERR_INVALID_ARGUMENT;
}

/* ── memory ─────────────────────────────────────────────────────────────── */

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
    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    sc_memory_t mem = sc_memory_create_from_config(alloc, &cfg, ws);
    if (!mem.vtable) {
        printf("Memory backend: none (not configured)\n");
        sc_config_deinit(&cfg);
        return SC_OK;
    }
    const char *name = mem.vtable->name ? mem.vtable->name(mem.ctx) : "unknown";
    const char *sub = argv[2];

    if (strcmp(sub, "stats") == 0 || strcmp(sub, "status") == 0) {
        size_t count = 0;
        if (mem.vtable->count)
            mem.vtable->count(mem.ctx, &count);
        bool healthy = mem.vtable->health_check ? mem.vtable->health_check(mem.ctx) : true;
        printf("Memory backend: %s\nEntry count: %zu\nHealth: %s\n", name, count,
               healthy ? "ok" : "error");
    } else if (strcmp(sub, "count") == 0) {
        size_t count = 0;
        if (mem.vtable->count)
            mem.vtable->count(mem.ctx, &count);
        printf("%zu\n", count);
    } else if (strcmp(sub, "list") == 0) {
        sc_memory_entry_t *entries = NULL;
        size_t count = 0;
        err = mem.vtable->list(mem.ctx, alloc, NULL, NULL, 0, &entries, &count);
        if (err != SC_OK || count == 0) {
            printf("No memory entries.\n");
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("  [%zu] %.*s: %.*s\n", i + 1, (int)entries[i].key_len,
                       entries[i].key ? entries[i].key : "",
                       (int)(entries[i].content_len > 80 ? 80 : entries[i].content_len),
                       entries[i].content ? entries[i].content : "");
                sc_memory_entry_free_fields(alloc, &entries[i]);
            }
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
    } else if (strcmp(sub, "search") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw memory search <query>\n");
            err = SC_ERR_INVALID_ARGUMENT;
            goto done;
        }
        sc_memory_entry_t *entries = NULL;
        size_t count = 0;
        err = mem.vtable->recall(mem.ctx, alloc, argv[3], strlen(argv[3]), 10, NULL, 0, &entries,
                                 &count);
        if (err != SC_OK || count == 0) {
            printf("No results for: %s\n", argv[3]);
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("  [%zu] %.*s: %.*s\n", i + 1, (int)entries[i].key_len,
                       entries[i].key ? entries[i].key : "",
                       (int)(entries[i].content_len > 80 ? 80 : entries[i].content_len),
                       entries[i].content ? entries[i].content : "");
                sc_memory_entry_free_fields(alloc, &entries[i]);
            }
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
    } else if (strcmp(sub, "get") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw memory get <key>\n");
            err = SC_ERR_INVALID_ARGUMENT;
            goto done;
        }
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
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw memory forget <key>\n");
            err = SC_ERR_INVALID_ARGUMENT;
            goto done;
        }
        bool deleted = false;
        err = mem.vtable->forget(mem.ctx, argv[3], strlen(argv[3]), &deleted);
        printf("%s: %s\n", argv[3], deleted ? "forgotten" : "not found");
    } else {
        fprintf(stderr, "Unknown memory subcommand: %s\n", sub);
        err = SC_ERR_INVALID_ARGUMENT;
    }
done:
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    sc_config_deinit(&cfg);
    return err;
}

/* ── workspace ───────────────────────────────────────────────────────────── */
sc_error_t cmd_workspace(sc_allocator_t *alloc, int argc, char **argv) {
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    const char *ws = (err == SC_OK && cfg.workspace_dir) ? cfg.workspace_dir : ".";

    if (argc < 3 || strcmp(argv[2], "show") == 0) {
        printf("Current workspace: %s\n", ws);
        if (err == SC_OK)
            sc_config_deinit(&cfg);
        return SC_OK;
    }
    if (strcmp(argv[2], "set") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw workspace set <path>\n");
            if (err == SC_OK)
                sc_config_deinit(&cfg);
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (err == SC_OK) {
            char json_buf[1024];
            size_t jp = 0;
            jp += (size_t)snprintf(json_buf + jp, sizeof(json_buf) - jp, "{\"workspace\":\"");
            const char *s = argv[3];
            for (; *s && jp + 4 < sizeof(json_buf); s++) {
                if (*s == '"' || *s == '\\')
                    json_buf[jp++] = '\\';
                json_buf[jp++] = *s;
            }
            jp += (size_t)snprintf(json_buf + jp, sizeof(json_buf) - jp, "\"}");
            sc_error_t pe = sc_config_parse_json(&cfg, json_buf, jp);
            if (pe == SC_OK) {
                sc_error_t se = sc_config_save(&cfg);
                if (se == SC_OK)
                    printf("Workspace set to: %s\n", argv[3]);
                else
                    fprintf(stderr, "Failed to save config: %s\n", sc_error_string(se));
            }
            sc_config_deinit(&cfg);
        }
        return SC_OK;
    }
    if (err == SC_OK)
        sc_config_deinit(&cfg);
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
    const char *prov = (err == SC_OK && cfg.default_provider) ? cfg.default_provider : "gemini";
    const char *backend = (err == SC_OK && cfg.memory.backend) ? cfg.memory.backend : "none";
    if (json_mode) {
        printf("{\"channels\":[\"cli\"],\"tools\":[\"shell\",\"file_read\",\"file_write\",\"file_"
               "edit\","
               "\"git\",\"web_search\",\"web_fetch\",\"memory_store\",\"memory_recall\"],"
               "\"providers\":[\"%s\"],\"memory\":\"%s\"}\n",
               prov, backend);
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
    if (err == SC_OK)
        sc_config_deinit(&cfg);
    return SC_OK;
}

/* ── models ─────────────────────────────────────────────────────────────── */
sc_error_t cmd_models(sc_allocator_t *alloc, int argc, char **argv) {
    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);

    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        if (err == SC_OK) {
            const char *prov = cfg.default_provider ? cfg.default_provider : "gemini";
            const char *model = cfg.default_model ? cfg.default_model : "(provider default)";
            printf("Active: provider=%s model=%s\n\n", prov, model);
            if (cfg.providers_len > 0) {
                printf("Configured providers:\n");
                for (size_t i = 0; i < cfg.providers_len; i++) {
                    printf("  %-16s\n", cfg.providers[i].name ? cfg.providers[i].name : "?");
                }
                printf("\n");
            }
        }
        printf("Known providers and default models:\n");
        printf("  %-16s %s\n", "Provider", "Default Model");
        printf("  %-16s %s\n", "--------", "-------------");
        printf("  %-16s %s\n", "openai", "gpt-4o");
        printf("  %-16s %s\n", "anthropic", "claude-sonnet-4-20250514");
        printf("  %-16s %s\n", "google", "gemini-2.0-flash");
        printf("  %-16s %s\n", "groq", "llama-3.3-70b-versatile");
        printf("  %-16s %s\n", "deepseek", "deepseek-chat");
        printf("  %-16s %s\n", "ollama", "(local)");
        printf("  %-16s %s\n", "openrouter", "(varies)");
        if (err == SC_OK)
            sc_config_deinit(&cfg);
        return SC_OK;
    }
    if (strcmp(argv[2], "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: seaclaw models info <model>\n");
            if (err == SC_OK)
                sc_config_deinit(&cfg);
            return SC_ERR_INVALID_ARGUMENT;
        }
        printf("Model: %s\n", argv[3]);
        if (err == SC_OK) {
            const char *prov = cfg.default_provider ? cfg.default_provider : "unknown";
            printf("Provider: %s\n", prov);
            sc_config_deinit(&cfg);
        } else {
            printf("Provider: unknown\n");
        }
        return SC_OK;
    }
    if (err == SC_OK)
        sc_config_deinit(&cfg);
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

/* ── sandbox ────────────────────────────────────────────────────────────── */
sc_error_t cmd_sandbox(sc_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;

    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "Failed to load config: %s\n", sc_error_string(err));
        return err;
    }

    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    sc_sandbox_backend_t backend = cfg.security.sandbox_config.backend;

    printf("Sandbox Configuration\n");
    printf("  backend:       %s\n", cfg.security.sandbox ? cfg.security.sandbox : "auto");
    printf("  configured:    %s\n",
           cfg.security.sandbox_config.enabled ? "yes (explicit)" : "auto-detect");

    /* Detect available backends */
    sc_sandbox_alloc_t sa = {.ctx = alloc->ctx, .alloc = alloc->alloc, .free = alloc->free};
    sc_available_backends_t avail = sc_sandbox_detect_available(ws, &sa);

    printf("\nAvailable Backends\n");
    struct {
        const char *name;
        bool available;
        const char *desc;
    } backends[] = {
#ifdef __APPLE__
        {"seatbelt", avail.seatbelt, "macOS kernel sandbox (SBPL)"},
#endif
#ifdef __linux__
        {"landlock", avail.landlock, "Linux kernel FS ACLs"},
        {"seccomp", avail.seccomp, "Linux syscall filtering"},
        {"landlock+seccomp", avail.landlock_seccomp, "Combined FS + syscall isolation"},
        {"firejail", avail.firejail, "User-space namespace sandbox"},
        {"bubblewrap", avail.bubblewrap, "User-space container sandbox"},
        {"firecracker", avail.firecracker, "KVM microVM isolation"},
#endif
#ifdef _WIN32
        {"appcontainer", avail.appcontainer, "Windows AppContainer + Job Object"},
#endif
        {"docker", avail.docker, "Docker container isolation"},
        {"wasi", avail.wasi, "WebAssembly capability sandbox"},
    };
    size_t n = sizeof(backends) / sizeof(backends[0]);
    for (size_t i = 0; i < n; i++) {
        printf("  %-20s %s  %s\n", backends[i].name,
               backends[i].available ? "[available]  " : "[unavailable]", backends[i].desc);
    }

    /* Show active sandbox */
    sc_sandbox_storage_t *st = sc_sandbox_storage_create(&sa);
    if (st) {
        sc_sandbox_t sb = sc_sandbox_create(backend, ws, st, &sa);
        printf("\nActive Sandbox\n");
        printf("  name:        %s\n", sc_sandbox_name(&sb));
        printf("  available:   %s\n", sc_sandbox_is_available(&sb) ? "yes" : "no");
        printf("  description: %s\n", sc_sandbox_description(&sb));
        printf("  apply:       %s\n",
               (sb.vtable && sb.vtable->apply) ? "kernel-level" : "argv-wrapping");
        sc_sandbox_storage_destroy(st, &sa);
    }

    /* Network proxy */
    printf("\nNetwork Proxy\n");
    if (cfg.security.sandbox_config.net_proxy.enabled) {
        printf("  enabled:  yes\n");
        printf("  deny_all: %s\n", cfg.security.sandbox_config.net_proxy.deny_all ? "yes" : "no");
        if (cfg.security.sandbox_config.net_proxy.proxy_addr)
            printf("  proxy:    %s\n", cfg.security.sandbox_config.net_proxy.proxy_addr);
        if (cfg.security.sandbox_config.net_proxy.allowed_domains_len > 0) {
            printf("  allowed:  ");
            for (size_t i = 0; i < cfg.security.sandbox_config.net_proxy.allowed_domains_len; i++) {
                if (i > 0)
                    printf(", ");
                printf("%s", cfg.security.sandbox_config.net_proxy.allowed_domains[i]);
            }
            printf("\n");
        }
    } else {
        printf("  enabled:  no\n");
    }

    sc_config_deinit(&cfg);
    return SC_OK;
}

/* ── update ─────────────────────────────────────────────────────────────── */
sc_error_t cmd_update(sc_allocator_t *alloc, int argc, char **argv) {
#ifdef SC_HAS_UPDATE
    (void)alloc;
    bool check_only = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            check_only = true;
    }
    const char *ver = sc_version_string();
    printf("SeaClaw v%s\n", ver ? ver : "0.3.0");

    char latest[64];
    sc_error_t err = sc_update_check(latest, sizeof(latest));
    if (err != SC_OK) {
        printf("Could not check for updates. Check https://github.com/sethdford/seaclaw/releases\n");
        return SC_OK;
    }

    const char *current = ver ? ver : "0.0.0";
    const char *remote = latest;
    if (remote[0] == 'v')
        remote++;
    if (strcmp(current, remote) == 0) {
        printf("Already up to date.\n");
        return SC_OK;
    }
    printf("Update available: %s -> %s\n", current, latest);
    if (check_only)
        return SC_OK;

    printf("Downloading update...\n");
    err = sc_update_apply();
    if (err != SC_OK)
        printf(
            "Update failed. Download manually from https://github.com/sethdford/seaclaw/releases\n");
    return SC_OK;
#else
    (void)alloc;
    (void)argc;
    (void)argv;
    fprintf(stderr, "[seaclaw] update support not built (compile with SC_ENABLE_UPDATE=ON)\n");
    return SC_ERR_NOT_SUPPORTED;
#endif
}
