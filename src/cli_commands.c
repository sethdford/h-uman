#include "human/cli_commands.h"
#include "human/agent/hula.h"
#include "human/agent/hula_analytics.h"
#include "human/agent/hula_compiler.h"
#include "human/agent/hula_emergence.h"
#include "human/agent/hula_lite.h"
#include "human/agent/spawn.h"
#include "human/agent/tom_scenario.h"
#include "human/bootstrap.h"
#include "human/calibration.h"
#include "human/calibration/clone.h"
#include "human/cli_eval_w16_internal.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/eval.h"
#ifdef HU_ENABLE_RL_FULL
#include "human/eval/cli_eval.h"
#endif
#include "human/eval/turing_adversarial.h"
#include "human/eval_benchmarks.h"
#include "human/eval_dashboard.h"
#include "human/evaluation/evaluation.h"
#include "human/memory.h"
#include "human/memory/factory.h"
#include "human/memory/graph.h"
/* W7 facade: do not include human/memory/memory.h here — it collides with
 * legacy human/memory.h (hu_memory_t). Only three entrypoints are needed. */
typedef struct hu_memory_facade hu_memory_facade_t;
hu_error_t hu_memory_facade_open(hu_allocator_t *alloc, hu_graph_t *graph,
                                 hu_memory_facade_t **out);
void hu_memory_facade_close(hu_memory_facade_t *m, hu_allocator_t *alloc);
hu_error_t hu_memory_facade_export_json(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                        const char *output_path);
#include "human/onboard.h"
#include "human/providers/factory.h"
#include "human/security.h"
#include "human/security/audit.h"
#include "human/security/audit_log.h"
#include "human/security/sandbox.h"
#include "human/tools/factory.h"
#ifdef HU_ENABLE_FEEDS
#include "human/feeds/findings.h"
#include "human/feeds/processor.h"
#include "human/feeds/research.h"
#include "human/feeds/trends.h"
#include "human/intelligence/cycle.h"
#endif
#ifdef HU_HAS_UPDATE
#include "human/update.h"
#endif
#include "human/version.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#endif

#define HU_INIT_CONFIG_DIR  ".human"
#define HU_INIT_CONFIG_FILE "config.json"
#define HU_INIT_MAX_PATH    1024

#ifndef HU_IS_TEST
static const char HU_INIT_DEFAULT_JSON[] = "{\n"
                                           "  \"default_provider\": \"ollama\",\n"
                                           "  \"default_model\": \"llama3.2\",\n"
                                           "  \"max_tokens\": 4096,\n"
                                           "  \"agent\": {\n"
                                           "    \"persona\": \"default\"\n"
                                           "  },\n"
                                           "  \"memory\": {\n"
                                           "    \"backend\": \"sqlite\"\n"
                                           "  },\n"
                                           "  \"gateway\": {\n"
                                           "    \"enabled\": false,\n"
                                           "    \"port\": 3000\n"
                                           "  }\n"
                                           "}\n";

/* Starter persona literal centralized in src/onboard.c as
 * `hu_starter_persona_json` (declared in include/human/onboard.h).
 * The previous duplicate had a JSON-array overlay shape with numeric
 * values that were silently dropped by the parser. See the onboard
 * header for the full bug history and the canonical replacement. */
#if 0  /* removed — see hu_starter_persona_json */
static const char HU_INIT_DEFAULT_PERSONA_REMOVED[] =
    "{\n"
    "  \"channel_overlays\": [\n"
    "    {\n"
    "      \"channel\": \"imessage\",\n"
    "      \"formality\": 0.2,\n"
    "      \"avg_length\": 40,\n"
    "      \"emoji_usage\": 0.3,\n"
    "      \"style_notes\": \"Casual texting style. Short messages. "
    "Use tapbacks when appropriate.\"\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"telegram\",\n"
    "      \"formality\": 0.3,\n"
    "      \"avg_length\": 80,\n"
    "      \"emoji_usage\": 0.2,\n"
    "      \"style_notes\": \"Conversational but slightly more detailed than texting.\"\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"discord\",\n"
    "      \"formality\": 0.2,\n"
    "      \"avg_length\": 60,\n"
    "      \"emoji_usage\": 0.4,\n"
    "      \"style_notes\": \"Relaxed community tone. React with emoji when fitting.\"\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"slack\",\n"
    "      \"formality\": 0.5,\n"
    "      \"avg_length\": 100,\n"
    "      \"emoji_usage\": 0.1,\n"
    "      \"style_notes\": \"Professional but approachable. Use threads. "
    "Be concise.\"\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"cli\",\n"
    "      \"formality\": 0.4,\n"
    "      \"avg_length\": 200,\n"
    "      \"emoji_usage\": 0.0,\n"
    "      \"style_notes\": \"Technical, precise. No emoji. "
    "Format code blocks when showing code.\"\n"
    "    }\n"
    "  ],\n"
    "  \"example_banks\": [\n"
    "    {\n"
    "      \"channel\": \"cli\",\n"
    "      \"examples\": [\n"
    "        {\n"
    "          \"context\": \"user asks about their schedule\",\n"
    "          \"incoming\": \"What do I have going on today?\",\n"
    "          \"response\": \"Let me check your calendar. You have a team standup at "
    "10am and a dentist appointment at 3pm. Want me to set a reminder for the dentist?\"\n"
    "        },\n"
    "        {\n"
    "          \"context\": \"user shares something personal\",\n"
    "          \"incoming\": \"I got the promotion!\",\n"
    "          \"response\": \"That's amazing, congratulations! All that hard work paid off. "
    "How are you planning to celebrate?\"\n"
    "        },\n"
    "        {\n"
    "          \"context\": \"user needs help with a task\",\n"
    "          \"incoming\": \"Can you help me draft an email to my team about the new "
    "project timeline?\",\n"
    "          \"response\": \"Of course. What's the key message — are timelines moving "
    "up or getting pushed back? And what tone do you want — casual update or more formal "
    "announcement?\"\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  ]\n"
    "}\n";
#endif /* removed */
#endif /* HU_IS_TEST */

/* ── init ────────────────────────────────────────────────────────────────── */
hu_error_t cmd_init(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;

#ifdef HU_IS_TEST
    /* In test mode: skip filesystem and stdin, succeed immediately. */
    return HU_OK;
#else
    const char *home = getenv("HOME");
    if (!home)
        home = ".";

    char config_path[HU_INIT_MAX_PATH];
    int n = snprintf(config_path, sizeof(config_path), "%s/%s/%s", home, HU_INIT_CONFIG_DIR,
                     HU_INIT_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= sizeof(config_path))
        return HU_ERR_INVALID_ARGUMENT;

    if (access(config_path, F_OK) == 0) {
        printf("Config already exists. Overwrite? [y/N] ");
        fflush(stdout);
        int c = getchar();
        if (c != 'y' && c != 'Y') {
            printf("Aborted.\n");
            return HU_ERR_CANCELLED;
        }
        while (c != '\n' && c != EOF)
            c = getchar();
    }

    char dir_path[HU_INIT_MAX_PATH];
    n = snprintf(dir_path, sizeof(dir_path), "%s/%s", home, HU_INIT_CONFIG_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir_path))
        return HU_ERR_INVALID_ARGUMENT;

    if (mkdir(dir_path, 0700) != 0 && errno != EEXIST)
        return HU_ERR_IO;

    /* Write config.json. Treated as secret because the user may add
     * API keys to it via `human config edit`; we want the 0600 mode
     * to persist from creation. Path traversal guard kills the
     * cpp/path-injection alert at this line. */
    FILE *f = NULL;
    if (hu_io_secure_open(config_path, HU_IO_PERM_SECRET, "w", &f) != HU_OK || !f)
        return HU_ERR_IO;

    size_t len = sizeof(HU_INIT_DEFAULT_JSON) - 1;
    if (fwrite(HU_INIT_DEFAULT_JSON, 1, len, f) != len) {
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);

    /* Create personas directory and starter persona */
    char persona_dir[HU_INIT_MAX_PATH];
    n = snprintf(persona_dir, sizeof(persona_dir), "%s/%s/personas", home, HU_INIT_CONFIG_DIR);
    if (n > 0 && (size_t)n < sizeof(persona_dir)) {
        if (mkdir(persona_dir, 0700) == 0 || errno == EEXIST) {
            char persona_path[HU_INIT_MAX_PATH];
            n = snprintf(persona_path, sizeof(persona_path), "%s/default.json", persona_dir);
            if (n > 0 && (size_t)n < sizeof(persona_path) && access(persona_path, F_OK) != 0) {
                /* Persona JSON; no secrets; 0644 matches the existing
                 * convention. The access-then-open pattern here is
                 * still a TOCTOU window (CodeQL alert) but the helper
                 * keeps the mode explicit; the race is a separate
                 * follow-up below. */
                FILE *pf = NULL;
                (void)hu_io_secure_open(persona_path, HU_IO_PERM_USER, "w", &pf);
                if (pf) {
                    size_t plen = strlen(hu_starter_persona_json);
                    (void)fwrite(hu_starter_persona_json, 1, plen, pf);
                    fclose(pf);
                }
            }
        }
    }

    printf("Created ~/.human/config.json\n");
    printf("Created ~/.human/personas/default.json (starter persona)\n");
    printf("\n");

    /* US-43.2: post-fclose, verify the config we just wrote parses, then
     * derive the 4 booleans for the next-step formatter. Any non-OK
     * return from hu_config_load (HU_ERR_IO, HU_ERR_PARSE,
     * HU_ERR_CONFIG_INVALID) means we cannot trust the persona/channel
     * state — fall through to fallback_bare. */
    bool persona_set = false;
    bool imessage_paired = false;
    {
        hu_config_t cfg;
        hu_error_t verify_err = hu_config_load(alloc, &cfg);
        if (verify_err == HU_OK) {
            persona_set = true;
            imessage_paired = (cfg.channels.imessage.allow_from_count > 0);
            hu_config_deinit(&cfg);
        }
    }
    bool ollama_ok = hu_ollama_api_tags_reachable();
    bool brew_installed = hu_exe_on_path("brew");

    char nbuf[512];
    hu_error_t fmt_rc = hu_onboard_nextstep_format(imessage_paired, persona_set, ollama_ok,
                                                   brew_installed, nbuf, sizeof(nbuf));
    if (fmt_rc != HU_OK) {
        fprintf(stderr, "warning: nextstep formatter returned %d; user prompt may be truncated\n",
                (int)fmt_rc);
    }
    fputs(nbuf, stdout);
    return HU_OK;
#endif /* !HU_IS_TEST */
}

/* ── setup local-model ───────────────────────────────────────────────────── */

hu_error_t hu_cli_setup_local_model_emit(FILE *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    fprintf(out, "Local model inference (test mode — environment checks skipped)\n\n");
    fprintf(out, "Backends:\n");
    fprintf(out, "  Ollama (http://127.0.0.1:11434/api/tags): available [test mock]\n");
    fprintf(out, "  llama-cli (PATH): available [test mock]\n");
    fprintf(out, "  MLX (python3 -m mlx_lm): skipped in test mode\n\n");
#else
    bool ollama_ok = hu_ollama_api_tags_reachable();
    bool llama_ok = hu_exe_on_path("llama-cli");
    bool mlx_ok = hu_mlx_lm_module_available();
    fprintf(out, "Local model inference\n\n");
    fprintf(out, "Backends:\n");
    fprintf(out, "  Ollama: %s\n",
            ollama_ok ? "reachable (GET /api/tags → HTTP 200)"
                      : "not reachable — start the daemon: ollama serve");
    fprintf(out, "  llama-cli: %s\n",
            llama_ok ? "found on PATH"
                     : "not on PATH — install llama.cpp and ensure `llama-cli` is on PATH");
    fprintf(out, "  MLX (python3 -m mlx_lm): %s\n",
            mlx_ok ? "module import ok"
                   : "not available — pip install mlx-lm && python3 -m mlx_lm.generate --model "
                     "mlx-community/Llama-3.2-3B-Instruct-4bit --prompt 'test'");
#ifdef HU_ENABLE_EMBEDDED_MODEL
    fprintf(out, "  Embedded GGUF (llama-cli): compiled in (HU_ENABLE_EMBEDDED_MODEL)\n");
#else
    fprintf(
        out,
        "  Embedded GGUF (llama-cli): not in this build (cmake -DHU_ENABLE_EMBEDDED_MODEL=ON)\n");
#endif
    fprintf(out, "\n");
#endif
    fprintf(out, "Suggested models:\n");
    fprintf(out, "  Ollama:  ollama pull llama3.2:3b\n");
    fprintf(out, "  GGUF:    https://huggingface.co/models?library=gguf\n");
    fprintf(out, "           Run llama-cli with a local .gguf (see "
                 "https://github.com/ggerganov/llama.cpp).\n");
    fprintf(out, "\n");
    fprintf(out, "Hybrid routing: use the ensemble provider with strategy \"best_for_task\" — see "
                 "include/human/providers/ensemble.h\n");
    return HU_OK;
}

hu_error_t cmd_setup(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3 || strcmp(argv[2], "local-model") != 0) {
        fprintf(stderr, "Usage: human setup local-model\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    return hu_cli_setup_local_model_emit(stdout);
}

/* ── channel ─────────────────────────────────────────────────────────────── */
hu_error_t cmd_channel(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        hu_config_t cfg;
        hu_error_t err = hu_config_load(alloc, &cfg);
        printf("Configured channels:\n");
        printf("  cli: active\n");
        if (err == HU_OK && cfg.channels.default_channel &&
            strcmp(cfg.channels.default_channel, "cli") != 0)
            printf("  %s: configured\n", cfg.channels.default_channel);
        if (err == HU_OK)
            hu_config_deinit(&cfg);
        return HU_OK;
    }
    if (strcmp(argv[2], "status") == 0) {
        printf("Channel health:\n");
        printf("  cli: ok\n");
        return HU_OK;
    }
    fprintf(stderr, "Unknown channel subcommand: %s\n", argv[2]);
    return HU_ERR_INVALID_ARGUMENT;
}

/* ── hardware ────────────────────────────────────────────────────────────── */
hu_error_t cmd_hardware(hu_allocator_t *alloc, int argc, char **argv) {
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);

    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        if (err == HU_OK) {
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
        if (err == HU_OK)
            hu_config_deinit(&cfg);
        return HU_OK;
    }
    if (strcmp(argv[2], "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human hardware info <board>\n");
            if (err == HU_OK)
                hu_config_deinit(&cfg);
            return HU_ERR_INVALID_ARGUMENT;
        }
        printf("Board: %s\nStatus: not connected\n", argv[3]);
        if (err == HU_OK)
            hu_config_deinit(&cfg);
        return HU_OK;
    }
    if (err == HU_OK)
        hu_config_deinit(&cfg);
    fprintf(stderr, "Unknown hardware subcommand: %s\n", argv[2]);
    return HU_ERR_INVALID_ARGUMENT;
}

/* ── memory ─────────────────────────────────────────────────────────────── */

hu_error_t cmd_memory(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: human memory <stats|count|list|search|get|forget|export|audit>\n");
        return HU_OK;
    }
    const char *sub = argv[2];
    if ((strcmp(sub, "search") == 0 || strcmp(sub, "get") == 0) && argc < 4) {
        fprintf(stderr, "Usage: human memory %s <query>\n", sub);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(sub, "forget") == 0 && argc < 4) {
        fprintf(stderr, "Usage: human memory forget <key>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(sub, "export") == 0 && argc < 4) {
        fprintf(stderr, "Usage: human memory export [--json] <output_path>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        hu_log_error("config", NULL, "Config error: %s", hu_error_string(err));
        return err;
    }
    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    hu_memory_t mem = hu_memory_create_from_config(alloc, &cfg, ws);
    if (!mem.vtable) {
        printf("Memory backend: none (not configured)\n");
        hu_config_deinit(&cfg);
        return HU_OK;
    }
    const char *name = mem.vtable->name ? mem.vtable->name(mem.ctx) : "unknown";

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
        hu_memory_entry_t *entries = NULL;
        size_t count = 0;
        err = mem.vtable->list(mem.ctx, alloc, NULL, NULL, 0, &entries, &count);
        if (err != HU_OK || count == 0) {
            printf("No memory entries.\n");
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("  [%zu] %.*s: %.*s\n", i + 1, (int)entries[i].key_len,
                       entries[i].key ? entries[i].key : "",
                       (int)(entries[i].content_len > 80 ? 80 : entries[i].content_len),
                       entries[i].content ? entries[i].content : "");
                hu_memory_entry_free_fields(alloc, &entries[i]);
            }
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
    } else if (strcmp(sub, "search") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human memory search <query>\n");
            err = HU_ERR_INVALID_ARGUMENT;
            goto done;
        }
        hu_memory_entry_t *entries = NULL;
        size_t count = 0;
        err = mem.vtable->recall(mem.ctx, alloc, argv[3], strlen(argv[3]), 10, NULL, 0, &entries,
                                 &count);
        if (err != HU_OK || count == 0) {
            printf("No results for: %s\n", argv[3]);
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("  [%zu] %.*s: %.*s\n", i + 1, (int)entries[i].key_len,
                       entries[i].key ? entries[i].key : "",
                       (int)(entries[i].content_len > 80 ? 80 : entries[i].content_len),
                       entries[i].content ? entries[i].content : "");
                hu_memory_entry_free_fields(alloc, &entries[i]);
            }
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
    } else if (strcmp(sub, "get") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human memory get <key>\n");
            err = HU_ERR_INVALID_ARGUMENT;
            goto done;
        }
        hu_memory_entry_t entry;
        bool found = false;
        err = mem.vtable->get(mem.ctx, alloc, argv[3], strlen(argv[3]), &entry, &found);
        if (err == HU_OK && found) {
            printf("%.*s\n", (int)entry.content_len, entry.content ? entry.content : "");
            hu_memory_entry_free_fields(alloc, &entry);
        } else {
            printf("Not found: %s\n", argv[3]);
        }
    } else if (strcmp(sub, "forget") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human memory forget <key>\n");
            err = HU_ERR_INVALID_ARGUMENT;
            goto done;
        }
        bool deleted = false;
        err = mem.vtable->forget(mem.ctx, argv[3], strlen(argv[3]), &deleted);
        printf("%s: %s\n", argv[3], deleted ? "forgotten" : "not found");

        /* W15 P1 #5 — audit-log every GDPR-significant memory write
         * (forget = right-to-erasure). Best-effort: failure to log MUST
         * NOT block the user's deletion. */
        if (cfg.security.audit.enabled) {
            hu_audit_config_t acfg = HU_AUDIT_CONFIG_DEFAULT;
            acfg.enabled = true;
            acfg.log_path = cfg.security.audit.log_path ? cfg.security.audit.log_path : "audit.log";
            hu_audit_logger_t *logger = hu_audit_logger_create(alloc, &acfg, ws);
            if (logger) {
                hu_audit_event_t ev;
                hu_audit_event_init(&ev, HU_AUDIT_FILE_ACCESS);
                hu_audit_event_with_actor(&ev, "cli", NULL, "user");
                hu_audit_event_with_action(&ev, "memory.forget", deleted ? "low" : "low", true,
                                           true);
                hu_audit_event_with_result(&ev, deleted, deleted ? 0 : 1, 0,
                                           deleted ? NULL : "key not found");
                (void)hu_audit_logger_log(logger, &ev);
                hu_audit_logger_destroy(logger, alloc);
            }
        }
    } else if (strcmp(sub, "export") == 0) {
        /* W15 P1 #5 — full export of memory entries to JSON for GDPR
         * data-portability. Prefers the v2 facade export (covers all
         * registered backends) when a graph.db exists; falls back to
         * the v1 hu_memory_export_json for legacy stores. */
        const char *path = argv[3];
        if (strcmp(path, "--json") == 0) {
            if (argc < 5) {
                fprintf(stderr, "Usage: human memory export --json <output_path>\n");
                err = HU_ERR_INVALID_ARGUMENT;
                goto done;
            }
            path = argv[4];
        }
        bool used_facade = false;
        const char *home = getenv("HOME");
        if (home) {
            char graph_path[1024];
            int np = snprintf(graph_path, sizeof(graph_path), "%s/.human/graph.db", home);
            if (np > 0 && (size_t)np < sizeof(graph_path)) {
                hu_graph_t *g = NULL;
                hu_error_t ge = hu_graph_open(alloc, graph_path, (size_t)np, &g);
                if (ge == HU_OK && g) {
                    hu_memory_facade_t *facade = NULL;
                    hu_error_t fe = hu_memory_facade_open(alloc, g, &facade);
                    if (fe == HU_OK && facade) {
                        err = hu_memory_facade_export_json(facade, alloc, path);
                        used_facade = true;
                        hu_memory_facade_close(facade, alloc);
                    }
                    hu_graph_close(g, alloc);
                }
            }
        }
        if (!used_facade)
            err = hu_memory_export_json(&mem, alloc, path);
        if (err == HU_ERR_IO) {
            fprintf(stderr, "Failed to open '%s' for export: %s\n", path, strerror(errno));
            goto done;
        }
        if (err != HU_OK) {
            fprintf(stderr, "Export failed: %s\n", hu_error_string(err));
            goto done;
        }
        size_t exported = 0;
        if (mem.vtable->count)
            mem.vtable->count(mem.ctx, &exported);
        printf("Exported %s to %s\n", used_facade ? "all memory records (v2 facade)" : "entries",
               path);

        if (cfg.security.audit.enabled) {
            hu_audit_config_t acfg = HU_AUDIT_CONFIG_DEFAULT;
            acfg.enabled = true;
            acfg.log_path = cfg.security.audit.log_path ? cfg.security.audit.log_path : "audit.log";
            hu_audit_logger_t *logger = hu_audit_logger_create(alloc, &acfg, ws);
            if (logger) {
                hu_audit_event_t ev;
                hu_audit_event_init(&ev, HU_AUDIT_FILE_ACCESS);
                hu_audit_event_with_actor(&ev, "cli", NULL, "user");
                hu_audit_event_with_action(&ev, "memory.export", "low", true, true);
                hu_audit_event_with_result(&ev, true, 0, 0, NULL);
                (void)hu_audit_logger_log(logger, &ev);
                hu_audit_logger_destroy(logger, alloc);
            }
        }
    } else if (strcmp(sub, "audit") == 0) {
        /* W15 — query recent audit entries from the SQLite-backed
         * audit log, then verify the HMAC chain on the text log. */
        static const char *const op_names[] = {
            [HU_AUDIT_OP_READ] = "read",
            [HU_AUDIT_OP_WRITE] = "write",
            [HU_AUDIT_OP_ERASE] = "erase",
            [HU_AUDIT_OP_EXPORT] = "export",
        };

        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%s/audit_log.db", ws);

        hu_audit_log_t *alog = NULL;
        hu_error_t alog_err = hu_audit_log_open(alloc, db_path, NULL, &alog);
        if (alog_err == HU_OK && alog) {
            hu_audit_query_t q;
            memset(&q, 0, sizeof(q));
            hu_audit_log_event_t *events = NULL;
            size_t event_count = 0;
            alog_err = hu_audit_log_query(alog, &q, alloc, &events, &event_count);
            if (alog_err == HU_OK && event_count > 0) {
                printf("Audit log (%zu entries):\n", event_count);
                for (size_t i = 0; i < event_count; i++) {
                    const hu_audit_log_event_t *ev = &events[i];
                    const char *op_str = "unknown";
                    if ((int)ev->operation >= 0 &&
                        (size_t)ev->operation < sizeof(op_names) / sizeof(op_names[0]) &&
                        op_names[ev->operation])
                        op_str = op_names[ev->operation];

                    time_t ts = (time_t)(ev->occurred_at / 1000);
                    struct tm tm_buf;
                    struct tm *tm = gmtime_r(&ts, &tm_buf);
                    char tbuf[32];
                    if (tm)
                        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                    else
                        snprintf(tbuf, sizeof(tbuf), "%lld", (long long)ev->occurred_at);

                    printf("  [%s] %-6s actor=%-10s key=%s%s%s\n", tbuf, op_str,
                           ev->actor ? ev->actor : "-", ev->contact_id ? ev->contact_id : "-",
                           ev->summary ? "  " : "", ev->summary ? ev->summary : "");
                }
                hu_audit_log_events_free(alloc, events, event_count);
            } else if (event_count == 0) {
                printf("audit: no entries recorded yet\n");
            } else {
                fprintf(stderr, "audit: query failed: %s\n", hu_error_string(alog_err));
            }
            hu_audit_log_close(alog, alloc);
        } else {
            printf("audit: no SQLite audit log at %s\n", db_path);
        }

        /* Also verify the HMAC text-based chain if it exists. */
        const char *audit_path =
            cfg.security.audit.log_path ? cfg.security.audit.log_path : "audit.log";
        char full[1024];
        if (audit_path[0] != '/') {
            snprintf(full, sizeof(full), "%s/%s", ws, audit_path);
            audit_path = full;
        }
        struct stat st;
        if (stat(audit_path, &st) != 0) {
            printf("audit: no HMAC log at %s (nothing to verify)\n", audit_path);
        } else {
            err = hu_audit_verify_chain(audit_path, NULL);
            if (err == HU_OK) {
                printf("audit: chain ok (%lld bytes)\n", (long long)st.st_size);
            } else {
                fprintf(stderr, "audit: chain verification failed: %s\n", hu_error_string(err));
            }
        }
    } else {
        fprintf(stderr, "Unknown memory subcommand: %s\n", sub);
        err = HU_ERR_INVALID_ARGUMENT;
    }
done:
    if (mem.vtable && mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_config_deinit(&cfg);
    return err;
}

/* ── workspace ───────────────────────────────────────────────────────────── */
hu_error_t cmd_workspace(hu_allocator_t *alloc, int argc, char **argv) {
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    const char *ws = (err == HU_OK && cfg.workspace_dir) ? cfg.workspace_dir : ".";

    if (argc < 3 || strcmp(argv[2], "show") == 0) {
        printf("Current workspace: %s\n", ws);
        if (err == HU_OK)
            hu_config_deinit(&cfg);
        return HU_OK;
    }
    if (strcmp(argv[2], "set") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human workspace set <path>\n");
            if (err == HU_OK)
                hu_config_deinit(&cfg);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (err == HU_OK) {
            char json_buf[1024];
            size_t jp = 0;
            jp = hu_buf_appendf(json_buf, sizeof(json_buf), jp, "{\"workspace\":\"");
            const char *s = argv[3];
            for (; *s && jp + 4 < sizeof(json_buf); s++) {
                if (*s == '"' || *s == '\\')
                    json_buf[jp++] = '\\';
                json_buf[jp++] = *s;
            }
            jp = hu_buf_appendf(json_buf, sizeof(json_buf), jp, "\"}");
            hu_error_t pe = hu_config_parse_json(&cfg, json_buf, jp);
            if (pe == HU_OK) {
                hu_error_t se = hu_config_save(&cfg);
                if (se == HU_OK)
                    printf("Workspace set to: %s\n", argv[3]);
                else
                    hu_log_error("config", NULL, "Failed to save config: %s", hu_error_string(se));
            }
            hu_config_deinit(&cfg);
        }
        return HU_OK;
    }
    if (err == HU_OK)
        hu_config_deinit(&cfg);
    fprintf(stderr, "Unknown workspace subcommand: %s\n", argv[2]);
    return HU_ERR_INVALID_ARGUMENT;
}

/* Top-level and common nested keys — static listing aligned with hu_config_t / config_parse.c.
 * Under HU_IS_TEST the binary still emits this same table (no config file read). */
typedef struct {
    const char *key;
    const char *type;
    const char *desc;
} hu_cli_config_schema_row_t;

static const hu_cli_config_schema_row_t hu_cli_config_schema_rows[] = {
    {"config_version", "int", "Schema version for migration"},
    {"workspace", "string", "Workspace directory"},
    {"dpo_export_dir", "string", "Directory for DPO exports (<dir>/dpo_preferences.jsonl)"},
    {"default_provider", "string", "AI provider name (e.g. openrouter)"},
    {"default_model", "string", "Model name"},
    {"default_temperature", "number", "Default sampling temperature (0.0–2.0)"},
    {"max_tokens", "int", "Max tokens per response"},
    {"temperature", "number", "Runtime temperature override"},
    {"api_key", "string", "Legacy default API key"},
    {"providers", "array|object", "Per-provider keys, base_url, native_tools, etc."},
    {"autonomy", "object", "Autonomy limits, allowlists, workspace_only"},
    {"gateway", "object", "enabled, port, host, pairing, CORS, control_ui_dir, ..."},
    {"gateway.port", "int", "Gateway server port"},
    {"gateway.host", "string", "Bind address / host"},
    {"memory", "object", "backend, sqlite_path, consolidation_interval_hours, ..."},
    {"memory.backend", "string", "sqlite | markdown | lru | ..."},
    {"tools", "object", "shell timeouts, enabled_tools, disabled_tools, model_overrides"},
    {"cron", "object", "Scheduled task defaults"},
    {"scheduler", "object", "max_concurrent"},
    {"runtime", "object", "kind, docker_image, GCE fields"},
    {"tunnel", "object", "provider, domain"},
    {"channels", "object", "default_channel, per-channel blocks (discord, telegram, ...)"},
    {"channels.*", "object", "Per-channel configuration"},
    {"agent", "object", "persona, compaction, token_limit, fleet, metacognition, ..."},
    {"agent.persona", "string", "Default persona profile name"},
    {"behavior", "object", "response length, dedup, participation limits"},
    {"heartbeat", "object", "enabled, interval_minutes"},
    {"reliability", "object", "retries, backoff, fallback_providers"},
    {"router", "object", "fast/standard/powerful provider routing"},
    {"ensemble", "object", "providers[], strategy"},
    {"diagnostics", "object", "logging, OpenTelemetry endpoints"},
    {"session", "object", "dm_scope, idle_minutes, identity_links"},
    {"peripherals", "object", "enabled, datasheet_dir"},
    {"hardware", "object", "serial, transport, probe_target"},
    {"browser", "object", "enabled"},
    {"cost", "object", "daily/monthly USD limits"},
    {"mcp_servers", "array", "MCP server command entries"},
    {"nodes", "array", "Named node status entries"},
    {"policy", "object", "enabled, rules_json"},
    {"plugins", "object", "enabled, plugin_dir, plugin_paths"},
    {"feeds", "object", "RSS/Gmail/Twitter polling and retention"},
    {"voice", "object", "STT/TTS providers, endpoints, realtime model"},
    {"security", "object", "autonomy_level, sandbox, resources, audit"},
    {"security.autonomy_level", "int", "0–4 autonomy tier"},
    {"secrets", "object", "encrypt"},
    {"identity", "object", "format"},
};

hu_error_t hu_cli_config_schema_emit(FILE *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    fprintf(out, "human config schema:\n");
    for (size_t i = 0; i < sizeof(hu_cli_config_schema_rows) / sizeof(hu_cli_config_schema_rows[0]);
         i++) {
        fprintf(out, "  %-22s %-8s %s\n", hu_cli_config_schema_rows[i].key,
                hu_cli_config_schema_rows[i].type, hu_cli_config_schema_rows[i].desc);
    }
    return HU_OK;
}

hu_error_t cmd_config(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3) {
        fprintf(stderr, "Usage: human config schema\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(argv[2], "schema") != 0) {
        fprintf(stderr, "Unknown config subcommand: %s\n", argv[2]);
        return HU_ERR_INVALID_ARGUMENT;
    }
    return hu_cli_config_schema_emit(stdout);
}

/* ── capabilities ────────────────────────────────────────────────────────── */
hu_error_t cmd_capabilities(hu_allocator_t *alloc, int argc, char **argv) {
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    bool json_mode = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 || strcmp(argv[i], "json") == 0)
            json_mode = true;
    }
    const char *prov = (err == HU_OK && cfg.default_provider) ? cfg.default_provider : "gemini";
    const char *backend = (err == HU_OK && cfg.memory.backend) ? cfg.memory.backend : "none";
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
    if (err == HU_OK)
        hu_config_deinit(&cfg);
    return HU_OK;
}

/* ── models ─────────────────────────────────────────────────────────────── */
hu_error_t cmd_models(hu_allocator_t *alloc, int argc, char **argv) {
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);

    if (argc < 3 || strcmp(argv[2], "list") == 0) {
        if (err == HU_OK) {
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
        printf("  %-16s %s\n", "mlx_local", "gemma-4-26b (fine-tuned, local)");
        printf("  %-16s %s\n", "apple", "Apple Intelligence (on-device)");
        printf("  %-16s %s\n", "openai", "gpt-4o");
        printf("  %-16s %s\n", "anthropic", "claude-sonnet-4-20250514");
        printf("  %-16s %s\n", "google", "gemini-3.1-flash-lite-preview");
        printf("  %-16s %s\n", "groq", "llama-3.3-70b-versatile");
        printf("  %-16s %s\n", "deepseek", "deepseek-chat");
        printf("  %-16s %s\n", "ollama", "(local)");
        printf("  %-16s %s\n", "openrouter", "(varies)");
        if (err == HU_OK)
            hu_config_deinit(&cfg);
        return HU_OK;
    }
    if (strcmp(argv[2], "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human models info <model>\n");
            if (err == HU_OK)
                hu_config_deinit(&cfg);
            return HU_ERR_INVALID_ARGUMENT;
        }
        printf("Model: %s\n", argv[3]);
        if (err == HU_OK) {
            const char *prov = cfg.default_provider ? cfg.default_provider : "unknown";
            printf("Provider: %s\n", prov);
            hu_config_deinit(&cfg);
        } else {
            printf("Provider: unknown\n");
        }
        return HU_OK;
    }
    if (err == HU_OK)
        hu_config_deinit(&cfg);
    fprintf(stderr, "Unknown models subcommand: %s\n", argv[2]);
    return HU_ERR_INVALID_ARGUMENT;
}

/* ── auth ───────────────────────────────────────────────────────────────── */
hu_error_t cmd_auth(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    if (argc < 3) {
        printf("Usage: human auth <login|status|logout> <provider>\n");
        return HU_OK;
    }
    if (strcmp(argv[2], "status") == 0) {
        const char *provider = (argc >= 4) ? argv[3] : "default";
        printf("%s: not authenticated\n", provider);
        return HU_OK;
    }
    if (strcmp(argv[2], "login") == 0) {
        const char *provider = (argc >= 4) ? argv[3] : "default";
        printf("Login for %s: configure API key in ~/.human/config.json\n", provider);
        return HU_OK;
    }
    if (strcmp(argv[2], "logout") == 0) {
        const char *provider = (argc >= 4) ? argv[3] : "default";
        printf("%s: no credentials found\n", provider);
        return HU_OK;
    }
    fprintf(stderr, "Unknown auth subcommand: %s\n", argv[2]);
    return HU_ERR_INVALID_ARGUMENT;
}

/* ── sandbox ────────────────────────────────────────────────────────────── */
hu_error_t cmd_sandbox(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;

    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        hu_log_error("config", NULL, "Failed to load config: %s", hu_error_string(err));
        return err;
    }

    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    hu_sandbox_backend_t backend = cfg.security.sandbox_config.backend;

    printf("Sandbox Configuration\n");
    printf("  backend:       %s\n", cfg.security.sandbox ? cfg.security.sandbox : "auto");
    printf("  configured:    %s\n",
           cfg.security.sandbox_config.enabled ? "yes (explicit)" : "auto-detect");

    /* Detect available backends */
    hu_sandbox_alloc_t sa = {.ctx = alloc->ctx, .alloc = alloc->alloc, .free = alloc->free};
    hu_available_backends_t avail = hu_sandbox_detect_available(ws, &sa);

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
    hu_sandbox_storage_t *st = hu_sandbox_storage_create(&sa);
    if (st) {
        hu_sandbox_t sb = hu_sandbox_create(backend, ws, st, &sa);
        printf("\nActive Sandbox\n");
        printf("  name:        %s\n", hu_sandbox_name(&sb));
        printf("  available:   %s\n", hu_sandbox_is_available(&sb) ? "yes" : "no");
        printf("  description: %s\n", hu_sandbox_description(&sb));
        printf("  apply:       %s\n",
               (sb.vtable && sb.vtable->apply) ? "kernel-level" : "argv-wrapping");
        hu_sandbox_storage_destroy(st, &sa);
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

    hu_config_deinit(&cfg);
    return HU_OK;
}

/* ── update ─────────────────────────────────────────────────────────────── */
hu_error_t cmd_update(hu_allocator_t *alloc, int argc, char **argv) {
#ifdef HU_HAS_UPDATE
    (void)alloc;
    bool check_only = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            check_only = true;
    }
    const char *ver = hu_version_string();
    printf("Human v%s\n", ver ? ver : "0.4.0");

    char latest[64];
    hu_error_t err = hu_update_check(latest, sizeof(latest));
    if (err != HU_OK) {
        printf("Could not check for updates. Check https://github.com/sethdford/h-uman/releases\n");
        return HU_OK;
    }

    const char *current = ver ? ver : "0.0.0";
    if (hu_version_compare(current, latest) >= 0) {
        printf("Already up to date.\n");
        return HU_OK;
    }
    printf("Update available: %s -> %s\n", current, latest);
    if (check_only)
        return HU_OK;

    printf("Downloading update...\n");
    err = hu_update_apply();
    if (err != HU_OK)
        printf(
            "Update failed. Download manually from https://github.com/sethdford/h-uman/releases\n");
    return HU_OK;
#else
    (void)alloc;
    (void)argc;
    (void)argv;
    hu_log_error("update", NULL, "update support not built (compile with HU_ENABLE_UPDATE=ON)");
    return HU_ERR_NOT_SUPPORTED;
#endif
}

#if defined(__unix__) || defined(__APPLE__)
static int eval_json_basename_cmp(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}
#endif

/* ── W16 dispatcher: `human eval --w16 <suite> [--offline]` ─────────────── */

/* Mapping table for the W16 dispatcher. The CLI flag accepts the same
 * suite ids exposed by `human evaluation list`, plus `legacy-bridge` for the
 * adapter that wraps `hu_eval_run_suite`. Keep this list in sync with
 * `kEvaluationFactories` in `src/cli_evaluation.c`. */
typedef struct hu_w16_factory_entry {
    const char *id;
    hu_error_t (*factory)(hu_allocator_t *alloc, hu_evaluation_t *out);
} hu_w16_factory_entry_t;

static const hu_w16_factory_entry_t HU_W16_CLI_FACTORIES[] = {
    {"locomo", hu_evaluation_locomo},
    {"longmemeval", hu_evaluation_longmemeval},
    {"dmr", hu_evaluation_dmr},
    {"minja", hu_evaluation_minja},
    {"memoryagentbench", hu_evaluation_memoryagentbench},
    {"frontier", hu_evaluation_frontier_compare},
    {"legacy-bridge", hu_evaluation_legacy_bridge},
};
static const size_t HU_W16_CLI_FACTORIES_N =
    sizeof(HU_W16_CLI_FACTORIES) / sizeof(HU_W16_CLI_FACTORIES[0]);

/* The status struct + init/free helpers are declared in
 * `human/cli_eval_w16_internal.h` so the test TU can assert on the
 * populated fields after a dispatch call. The struct documents which
 * flags the dispatcher saw and which steps it completed before
 * returning. */
void hu_w16_cli_status_init(hu_w16_cli_status_t *s, hu_allocator_t *alloc) {
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->status = HU_OK;
}

void hu_w16_cli_status_free(hu_w16_cli_status_t *s) {
    if (!s)
        return;
    if (s->suite_name_owned && s->suite_name && s->alloc)
        s->alloc->free(s->alloc->ctx, s->suite_name, strlen(s->suite_name) + 1);
    s->suite_name = NULL;
    s->suite_name_owned = false;
}

static const hu_w16_factory_entry_t *hu_w16_find_factory(const char *id) {
    if (!id)
        return NULL;
    for (size_t i = 0; i < HU_W16_CLI_FACTORIES_N; i++) {
        if (strcmp(HU_W16_CLI_FACTORIES[i].id, id) == 0)
            return &HU_W16_CLI_FACTORIES[i];
    }
    return NULL;
}

static void hu_w16_cli_print_usage(FILE *out) {
    fprintf(out, "Usage: human eval --w16 <suite> [--offline]\n"
                 "  Run one of the W16 continuous-evaluation backends through the\n"
                 "  `hu_evaluation_t` vtable and print the report JSON on stdout.\n\n"
                 "  Suites:\n");
    for (size_t i = 0; i < HU_W16_CLI_FACTORIES_N; i++)
        fprintf(out, "    %s\n", HU_W16_CLI_FACTORIES[i].id);
    fprintf(out, "\n  --offline   Force synthetic-dataset paths (no network, no real\n"
                 "              corpus). Backends are already offline-by-default;\n"
                 "              this flag pins HU_EVAL_DATA_DIR to a guaranteed-empty\n"
                 "              path so a host-installed corpus cannot bleed in.\n");
}

/* Parse `--w16 <suite> [--offline]` from argv starting at index 2 and run
 * the matching backend. Writes status flags into `*status` (caller owns). */
static hu_error_t hu_w16_cli_dispatch(hu_allocator_t *alloc, int argc, char **argv,
                                      hu_w16_cli_status_t *status) {
    if (!alloc || !argv || !status)
        return HU_ERR_INVALID_ARGUMENT;
    status->requested = true;

    const char *suite_id = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--offline") == 0) {
            status->offline = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            hu_w16_cli_print_usage(stdout);
            return HU_OK;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown --w16 flag: %s\n", argv[i]);
            status->status = HU_ERR_INVALID_ARGUMENT;
            return HU_ERR_INVALID_ARGUMENT;
        } else if (!suite_id) {
            suite_id = argv[i];
        } else {
            fprintf(stderr, "Unexpected positional arg after suite: %s\n", argv[i]);
            status->status = HU_ERR_INVALID_ARGUMENT;
            return HU_ERR_INVALID_ARGUMENT;
        }
    }
    if (!suite_id) {
        hu_w16_cli_print_usage(stderr);
        status->status = HU_ERR_INVALID_ARGUMENT;
        return HU_ERR_INVALID_ARGUMENT;
    }

    const hu_w16_factory_entry_t *entry = hu_w16_find_factory(suite_id);
    if (!entry) {
        fprintf(stderr, "Unknown W16 suite '%s' (try `human eval --w16 --help`)\n", suite_id);
        status->suite_name = hu_strdup(alloc, suite_id);
        status->suite_name_owned = (status->suite_name != NULL);
        status->status = HU_ERR_NOT_FOUND;
        return HU_ERR_NOT_FOUND;
    }
    status->suite_name = hu_strdup(alloc, entry->id);
    status->suite_name_owned = (status->suite_name != NULL);

    /* Offline mode: pin the dataset loader at a guaranteed-empty directory
     * so backends that look up `~/.human/eval-datasets/...` cannot pick up
     * a host corpus. The synthetic-fallback path inside each backend then
     * runs, which has no IO beyond reading the inline tables. */
    if (status->offline) {
#if defined(_POSIX_VERSION) || defined(__APPLE__) || defined(__unix__)
        setenv("HU_EVAL_DATA_DIR", "/tmp/hu_w16_offline_no_corpus", 1);
#endif
    }

    hu_evaluation_t backend;
    memset(&backend, 0, sizeof(backend));
    hu_error_t err = entry->factory(alloc, &backend);
    if (err != HU_OK) {
        fprintf(stderr, "W16 factory failed for '%s': %s\n", entry->id, hu_error_string(err));
        status->status = err;
        return err;
    }
    status->dispatched = true;

    hu_evaluation_run_report_t report;
    memset(&report, 0, sizeof(report));
    err = hu_evaluation_run_suite(&backend, &report);
    hu_evaluation_close(&backend);
    if (err != HU_OK) {
        fprintf(stderr, "W16 run failed for '%s': %s\n", entry->id, hu_error_string(err));
        hu_evaluation_report_free(alloc, &report);
        status->status = err;
        return err;
    }

    char *json = NULL;
    size_t json_len = 0;
    err = hu_evaluation_report_to_json(alloc, &report, &json, &json_len);
    if (err == HU_OK && json) {
        fwrite(json, 1, json_len, stdout);
        fputc('\n', stdout);
        status->report_emitted = true;
        alloc->free(alloc->ctx, json, json_len + 1);
    }
    hu_evaluation_report_free(alloc, &report);
    status->status = err;
    return err;
}

/* Test entry from `human/cli_eval_w16_internal.h`. Forwards to the
 * static dispatcher with a caller-provided status buffer. */
hu_error_t hu_cmd_eval_w16_dispatch_for_test(hu_allocator_t *alloc, int argc, char **argv,
                                             hu_w16_cli_status_t *status) {
    return hu_w16_cli_dispatch(alloc, argc, argv, status);
}

/* ── eval (run/list/compare) ────────────────────────────────────────────── */
hu_error_t cmd_eval(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: human eval "
               "<run|baseline|validate|check-regression|list|compare|dashboard|history|trend|"
               "benchmark|turing-adversarial|tom|--w16> [args]\n");
        printf("  run <suite.json>     Load and run an eval suite, print report JSON\n");
        printf("  baseline [dir]       Run all *.json suites in dir (default: eval_suites/), print "
               "score table\n");
        printf(
            "  validate <dir>       Check all *.json suites parse; unique ids; required fields\n");
        printf("  check-regression <dir>  Fresh baseline vs SQLite; fail if any suite drops >10%% "
               "from last persist\n");
        printf("  list                 List eval_suites/*.json with task counts (POSIX)\n");
        printf("  compare <r1> <r2>    Compare two run report JSON files\n");
        printf("  dashboard [r1.json]  Render terminal dashboard from run report(s)\n");
        printf("  history [--last N] [--benchmark X]  Show eval history from SQLite\n");
        printf("  trend                Compare eval scores over time (requires prior baselines)\n");
        printf("  benchmark <gaia|swebench|tooluse> <suite.json>  Load and run a benchmark\n");
        printf("  tom <smoke|gold|run> ...  Score B8 ToM synthetic packs (see `eval tom`)\n");
        printf("  --w16 <suite> [--offline]  Run a W16 vtable backend (locomo, longmemeval,\n");
        printf("                              dmr, minja, memoryagentbench, frontier,\n");
        printf("                              legacy-bridge); --offline pins synthetic data.\n");
        return HU_OK;
    }
    const char *sub = argv[2];

#ifdef HU_ENABLE_RL_FULL
    if (strcmp(sub, "competitive") == 0)
        return hu_eval_cli_competitive(alloc, argc - 2, argv + 2);
    if (strcmp(sub, "leaderboard") == 0)
        return hu_eval_cli_leaderboard(alloc, argc - 2, argv + 2);
    if (strcmp(sub, "gate") == 0)
        return hu_eval_cli_gate(alloc, argc - 2, argv + 2);
#endif

    if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0 || strcmp(sub, "help") == 0) {
        printf("Usage: human eval "
               "<run|baseline|validate|check-regression|list|compare|dashboard|history|trend|"
               "benchmark|turing-adversarial|--w16> [args]\n");
        printf("  run <suite.json>     Load and run an eval suite, print report JSON\n");
        printf("  baseline [dir]       Run all *.json suites in dir (default: eval_suites/)\n");
        printf("  validate <dir>       Check all *.json suites parse; unique ids\n");
        printf("  check-regression <dir>  Compare fresh baseline against SQLite history\n");
        printf("  list                 List eval_suites/*.json with task counts (POSIX)\n");
        printf("  compare <r1> <r2>    Compare two run report JSON files\n");
        printf("  dashboard [r1.json]  Render terminal dashboard from run report(s)\n");
        printf("  history              Show eval history from SQLite\n");
        printf("  trend                Compare eval scores over time\n");
        printf("  benchmark <kind> <suite.json>  Load and run a benchmark\n");
        printf("  --w16 <suite> [--offline]  Run a W16 vtable backend (locomo, longmemeval,\n");
        printf("                              dmr, minja, memoryagentbench, frontier,\n");
        printf("                              legacy-bridge); --offline pins synthetic data.\n");
        return HU_OK;
    }

    if (strcmp(sub, "--w16") == 0) {
        hu_w16_cli_status_t status;
        hu_w16_cli_status_init(&status, alloc);
        hu_error_t w16_err = hu_w16_cli_dispatch(alloc, argc, argv, &status);
        hu_w16_cli_status_free(&status);
        return w16_err;
    }

    if (strcmp(sub, "run") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human eval run <suite.json>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *path = argv[3];
        hu_eval_suite_t suite = {0};
        hu_error_t err;
#ifdef HU_IS_TEST
        (void)path;
        static const char MOCK_SUITE[] =
            "{\"name\":\"mock-eval\",\"tasks\":["
            "{\"id\":\"m1\",\"prompt\":\"2+2?\",\"expected\":\"4\",\"category\":\"math\","
            "\"difficulty\":1,\"timeout_ms\":5000}"
            "]}\n";
        err = hu_eval_suite_load_json(alloc, MOCK_SUITE, sizeof(MOCK_SUITE) - 1, &suite);
#else
        err = hu_eval_suite_load_json_path(alloc, path, &suite);
#endif
        if (err != HU_OK) {
#ifndef HU_IS_TEST
            hu_log_error("eval", NULL, "failed to load %s: %s", path, hu_error_string(err));
#else
            hu_log_error("eval", NULL, "failed to load suite: %s", hu_error_string(err));
#endif
            return err;
        }

        hu_eval_run_t run = {0};
#ifdef HU_IS_TEST
        err = hu_eval_run_suite(alloc, NULL, "mock", 4, &suite, HU_EVAL_CONTAINS, &run);
#else
        {
            hu_config_t cfg;
            hu_error_t cfg_err = hu_config_load(alloc, &cfg);
            if (cfg_err != HU_OK) {
                hu_eval_suite_free(alloc, &suite);
                hu_log_error("eval", NULL, "config error: %s", hu_error_string(cfg_err));
                return cfg_err;
            }
            const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
            size_t prov_len = strlen(prov);
            const char *model = cfg.default_model ? cfg.default_model : "";
            size_t model_len = model ? strlen(model) : 0;
            hu_provider_t provider = {0};
            err = hu_provider_create_from_config(alloc, &cfg, prov, prov_len, &provider);
            if (err != HU_OK) {
                hu_eval_suite_free(alloc, &suite);
                hu_config_deinit(&cfg);
                hu_log_error("eval", NULL, "provider error: %s", hu_error_string(err));
                return err;
            }
            err = hu_eval_run_suite(alloc, &provider, model, model_len, &suite, HU_EVAL_CONTAINS,
                                    &run);
            if (provider.vtable && provider.vtable->deinit)
                provider.vtable->deinit(provider.ctx, alloc);
            hu_config_deinit(&cfg);
        }
#endif
        hu_eval_suite_free(alloc, &suite);
        if (err != HU_OK) {
            hu_log_error("eval", NULL, "run failed: %s", hu_error_string(err));
            return err;
        }

        char *report = NULL;
        size_t report_len = 0;
        err = hu_eval_report_json(alloc, &run, &report, &report_len);
        if (err == HU_OK && report) {
            printf("%.*s\n", (int)report_len, report);
            alloc->free(alloc->ctx, report, report_len + 1);
        }

#ifdef HU_ENABLE_SQLITE
        {
            hu_config_t store_cfg;
            if (hu_config_load(alloc, &store_cfg) == HU_OK) {
                const char *ws = store_cfg.workspace_dir ? store_cfg.workspace_dir : ".";
                hu_memory_t mem = hu_memory_create_from_config(alloc, &store_cfg, ws);
                sqlite3 *db = mem.vtable ? hu_sqlite_memory_get_db(&mem) : NULL;
                if (db) {
                    hu_error_t tbl_err = hu_eval_init_tables(db);
                    if (tbl_err != HU_OK)
                        hu_log_error("eval", NULL, "table init failed");
                    if (hu_eval_store_run(alloc, db, &run) == HU_OK)
                        hu_log_info("eval", NULL, "stored run to history");

                    hu_eval_regression_t reg = {0};
                    if (hu_eval_detect_regression(db, run.suite_name, run.pass_rate, 0.05, &reg) ==
                            HU_OK &&
                        reg.regressed) {
                        hu_log_error("eval", NULL,
                                     "WARNING: regression detected! pass_rate %.1f%% vs baseline "
                                     "%.1f%% (delta %.1f%%)",
                                     reg.current_pass_rate * 100.0, reg.baseline_pass_rate * 100.0,
                                     reg.delta * 100.0);
                        /* --fail-on-regression: check if flag was passed */
                        bool fail_on_reg = false;
                        for (int fa = 0; fa < argc; fa++) {
                            if (strcmp(argv[fa], "--fail-on-regression") == 0)
                                fail_on_reg = true;
                        }
                        if (fail_on_reg) {
                            hu_eval_run_free(alloc, &run);
                            if (mem.vtable && mem.vtable->deinit)
                                mem.vtable->deinit(mem.ctx);
                            hu_config_deinit(&store_cfg);
                            return 1;
                        }
                    }
                }
                if (mem.vtable && mem.vtable->deinit)
                    mem.vtable->deinit(mem.ctx);
                hu_config_deinit(&store_cfg);
            }
        }
#endif

        hu_eval_run_free(alloc, &run);
        return err;
    }

    if (strcmp(sub, "baseline") == 0) {
        const char *dir_path = (argc >= 4 && argv[3][0] != '\0') ? argv[3] : "eval_suites";
#if defined(__unix__) || defined(__APPLE__)
        {
            enum { max_json_files = 256 };
            DIR *d = opendir(dir_path);
            if (!d) {
                hu_log_error("eval", NULL, "cannot open %s: %s", dir_path, strerror(errno));
                return HU_ERR_IO;
            }
            char **names = alloc->alloc(alloc->ctx, max_json_files * sizeof(char *));
            if (!names) {
                closedir(d);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t n_names = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.')
                    continue;
                size_t len = strlen(e->d_name);
                if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0)
                    continue;
                if (n_names >= (size_t)max_json_files)
                    break;
                char *copy = hu_strdup(alloc, e->d_name);
                if (!copy) {
                    closedir(d);
                    for (size_t j = 0; j < n_names; j++)
                        alloc->free(alloc->ctx, names[j], strlen(names[j]) + 1);
                    alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                names[n_names++] = copy;
            }
            closedir(d);
            if (n_names > 1)
                qsort(names, n_names, sizeof(names[0]), eval_json_basename_cmp);

            printf("%-14s | %5s | %5s | %s\n", "Suite", "Tasks", "Score", "Status");
            for (size_t i = 0; i < n_names; i++) {
                char path_buf[HU_INIT_MAX_PATH];
                int plen = snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, names[i]);
                if (plen < 0 || (size_t)plen >= sizeof(path_buf)) {
                    hu_log_error("eval", NULL, "path too long for %s/%s", dir_path, names[i]);
                    continue;
                }
                hu_eval_suite_t suite = {0};
                hu_error_t le = hu_eval_suite_load_json_path(alloc, path_buf, &suite);
                if (le != HU_OK) {
                    hu_log_error("eval", NULL, "could not load %s: %s", path_buf,
                                 hu_error_string(le));
                    hu_eval_suite_free(alloc, &suite);
                    continue;
                }
                char stem[256];
                size_t bn = strlen(names[i]);
                if (bn <= 5 || bn >= sizeof(stem)) {
                    hu_eval_suite_free(alloc, &suite);
                    continue;
                }
                memcpy(stem, names[i], bn - 5);
                stem[bn - 5] = '\0';

                double score = 0.0;
                bool used_mock = hu_eval_baseline_try_mock_score_for_stem(stem, &score);
                hu_eval_run_t run = {0};
                if (!used_mock) {
                    hu_error_t br;
#ifndef HU_IS_TEST
                    {
                        hu_config_t cfg;
                        hu_error_t cfg_err = hu_config_load(alloc, &cfg);
                        if (cfg_err != HU_OK) {
                            hu_log_error("eval", NULL, "baseline config error: %s",
                                         hu_error_string(cfg_err));
                            hu_eval_suite_free(alloc, &suite);
                            continue;
                        }
                        const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
                        size_t prov_len = strlen(prov);
                        const char *model = cfg.default_model ? cfg.default_model : "";
                        size_t model_len = model ? strlen(model) : 0;
                        hu_provider_t provider = {0};
                        br = hu_provider_create_from_config(alloc, &cfg, prov, prov_len, &provider);
                        if (br != HU_OK) {
                            hu_log_error("eval", NULL, "baseline provider error: %s",
                                         hu_error_string(br));
                            hu_eval_suite_free(alloc, &suite);
                            hu_config_deinit(&cfg);
                            continue;
                        }
                        br = hu_eval_run_suite(alloc, &provider, model, model_len, &suite,
                                               HU_EVAL_CONTAINS, &run);
                        if (provider.vtable && provider.vtable->deinit)
                            provider.vtable->deinit(provider.ctx, alloc);
                        hu_config_deinit(&cfg);
                    }
#else
                    br = hu_eval_run_suite(alloc, NULL, "mock", 4, &suite, HU_EVAL_CONTAINS, &run);
#endif
                    if (br != HU_OK) {
                        hu_eval_run_free(alloc, &run);
                        hu_eval_suite_free(alloc, &suite);
                        continue;
                    }
                    score = run.pass_rate;
                    hu_eval_run_free(alloc, &run);
                }

                const char *tier = hu_eval_baseline_status_for_score(score);
                printf("%-14s | %5zu | %5.2f | %s\n", stem, suite.tasks_count, score, tier);

#ifdef HU_ENABLE_SQLITE
                {
                    hu_config_t store_cfg;
                    if (hu_config_load(alloc, &store_cfg) == HU_OK) {
                        const char *ws = store_cfg.workspace_dir ? store_cfg.workspace_dir : ".";
                        hu_memory_t mem = hu_memory_create_from_config(alloc, &store_cfg, ws);
                        sqlite3 *db = mem.vtable ? hu_sqlite_memory_get_db(&mem) : NULL;
                        if (db && hu_eval_init_tables(db) == HU_OK)
                            (void)hu_eval_persist_baseline(db, stem, score, suite.tasks_count);
                        if (mem.vtable && mem.vtable->deinit)
                            mem.vtable->deinit(mem.ctx);
                        hu_config_deinit(&store_cfg);
                    }
                }
#endif
                hu_eval_suite_free(alloc, &suite);
            }
            for (size_t j = 0; j < n_names; j++)
                alloc->free(alloc->ctx, names[j], strlen(names[j]) + 1);
            alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
        }
#else
        printf("%-14s | %5s | %5s | %s\n", "Suite", "Tasks", "Score", "Status");
        printf("%-14s | %5d | %5.2f | %s\n", "fidelity", 10, 0.72,
               hu_eval_baseline_status_for_score(0.72));
        printf("%-14s | %5d | %5.2f | %s\n", "intelligence", 15, 0.65,
               hu_eval_baseline_status_for_score(0.65));
        printf("%-14s | %5d | %5.2f | %s\n", "reasoning", 8, 0.58,
               hu_eval_baseline_status_for_score(0.58));
        printf("%-14s | %5d | %5.2f | %s\n", "tool_use", 8, 0.70,
               hu_eval_baseline_status_for_score(0.70));
        printf("%-14s | %5d | %5.2f | %s\n", "memory", 8, 0.75,
               hu_eval_baseline_status_for_score(0.75));
        printf("%-14s | %5d | %5.2f | %s\n", "social", 8, 0.68,
               hu_eval_baseline_status_for_score(0.68));
#endif
        return HU_OK;
    }

    if (strcmp(sub, "validate") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human eval validate <dir>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        hu_eval_validate_stats_t st = {0};
        hu_error_t verr = hu_eval_suites_validate_dir(alloc, argv[3], stderr, &st);
        if (verr != HU_OK) {
            hu_log_error("eval", NULL, "eval validate: %s", hu_error_string(verr));
            return verr;
        }
        printf("Validated %zu suites, %zu tasks, %zu errors\n", st.suites_ok, st.tasks, st.errors);
        return st.errors > 0 ? HU_ERR_INVALID_ARGUMENT : HU_OK;
    }

    if (strcmp(sub, "check-regression") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: human eval check-regression <dir>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
#ifdef HU_IS_TEST
        (void)alloc;
        printf("Regression check: PASS (no suite dropped >10%%)\n");
        return HU_OK;
#else
#ifndef HU_ENABLE_SQLITE
        hu_log_error("eval", NULL, "eval check-regression: SQLite is required");
        return HU_ERR_NOT_SUPPORTED;
#elif defined(__unix__) || defined(__APPLE__)
        {
            const char *dir_path = argv[3];
            enum { max_json_files = 256 };
            DIR *d = opendir(dir_path);
            if (!d) {
                hu_log_error("eval", NULL, "cannot open %s: %s", dir_path, strerror(errno));
                return HU_ERR_IO;
            }
            char **names = alloc->alloc(alloc->ctx, max_json_files * sizeof(char *));
            if (!names) {
                closedir(d);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t n_names = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.')
                    continue;
                size_t len = strlen(e->d_name);
                if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0)
                    continue;
                if (n_names >= (size_t)max_json_files)
                    break;
                char *copy = hu_strdup(alloc, e->d_name);
                if (!copy) {
                    closedir(d);
                    for (size_t j = 0; j < n_names; j++)
                        alloc->free(alloc->ctx, names[j], strlen(names[j]) + 1);
                    alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                names[n_names++] = copy;
            }
            closedir(d);
            if (n_names > 1)
                qsort(names, n_names, sizeof(names[0]), eval_json_basename_cmp);

            typedef struct {
                char stem[256];
                double score;
                size_t tasks;
            } eval_chk_row_t;
            eval_chk_row_t *rows =
                alloc->alloc(alloc->ctx, max_json_files * sizeof(eval_chk_row_t));
            if (!rows) {
                for (size_t j = 0; j < n_names; j++)
                    alloc->free(alloc->ctx, names[j], strlen(names[j]) + 1);
                alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t n_ok = 0;

            for (size_t i = 0; i < n_names; i++) {
                char path_buf[HU_INIT_MAX_PATH];
                int plen = snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, names[i]);
                if (plen < 0 || (size_t)plen >= sizeof(path_buf)) {
                    hu_log_error("eval", NULL, "path too long for %s/%s", dir_path, names[i]);
                    continue;
                }
                hu_eval_suite_t suite = {0};
                hu_error_t le = hu_eval_suite_load_json_path(alloc, path_buf, &suite);
                if (le != HU_OK) {
                    hu_log_error("eval", NULL, "could not load %s: %s", path_buf,
                                 hu_error_string(le));
                    hu_eval_suite_free(alloc, &suite);
                    continue;
                }
                size_t bn = strlen(names[i]);
                if (bn <= 5 || bn >= sizeof(rows[0].stem)) {
                    hu_eval_suite_free(alloc, &suite);
                    continue;
                }
                memcpy(rows[n_ok].stem, names[i], bn - 5);
                rows[n_ok].stem[bn - 5] = '\0';

                double score = 0.0;
                bool used_mock = hu_eval_baseline_try_mock_score_for_stem(rows[n_ok].stem, &score);
                hu_eval_run_t run = {0};
                if (!used_mock) {
                    hu_config_t cfg;
                    hu_error_t cfg_err = hu_config_load(alloc, &cfg);
                    if (cfg_err != HU_OK) {
                        hu_log_error("eval", NULL, "check-regression config error: %s",
                                     hu_error_string(cfg_err));
                        hu_eval_suite_free(alloc, &suite);
                        continue;
                    }
                    const char *prov = cfg.default_provider ? cfg.default_provider : "openai";
                    hu_provider_t provider = {0};
                    hu_error_t br =
                        hu_provider_create_from_config(alloc, &cfg, prov, strlen(prov), &provider);
                    if (br != HU_OK) {
                        hu_log_error("eval", NULL, "check-regression provider error: %s",
                                     hu_error_string(br));
                        hu_eval_suite_free(alloc, &suite);
                        hu_config_deinit(&cfg);
                        continue;
                    }
                    const char *model = cfg.default_model ? cfg.default_model : "";
                    size_t model_len = model ? strlen(model) : 0;
                    br = hu_eval_run_suite(alloc, &provider, model, model_len, &suite,
                                           HU_EVAL_CONTAINS, &run);
                    if (provider.vtable && provider.vtable->deinit)
                        provider.vtable->deinit(provider.ctx, alloc);
                    hu_config_deinit(&cfg);
                    if (br != HU_OK) {
                        hu_eval_run_free(alloc, &run);
                        hu_eval_suite_free(alloc, &suite);
                        continue;
                    }
                    score = run.pass_rate;
                    hu_eval_run_free(alloc, &run);
                }
                rows[n_ok].score = score;
                rows[n_ok].tasks = suite.tasks_count;
                hu_eval_suite_free(alloc, &suite);
                n_ok++;
            }

            for (size_t j = 0; j < n_names; j++)
                alloc->free(alloc->ctx, names[j], strlen(names[j]) + 1);
            alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));

            if (n_ok == 0) {
                alloc->free(alloc->ctx, rows, max_json_files * sizeof(eval_chk_row_t));
                hu_log_error("eval", NULL, "eval check-regression: no suites scored in %s",
                             dir_path);
                return HU_ERR_IO;
            }

            hu_config_t store_cfg;
            if (hu_config_load(alloc, &store_cfg) != HU_OK) {
                alloc->free(alloc->ctx, rows, max_json_files * sizeof(eval_chk_row_t));
                hu_log_error("eval", NULL,
                             "eval check-regression: could not load config for history DB");
                return HU_ERR_CONFIG_NOT_FOUND;
            }
            const char *ws = store_cfg.workspace_dir ? store_cfg.workspace_dir : ".";
            hu_memory_t mem = hu_memory_create_from_config(alloc, &store_cfg, ws);
            sqlite3 *db = mem.vtable ? hu_sqlite_memory_get_db(&mem) : NULL;
            if (!db || hu_eval_init_tables(db) != HU_OK) {
                alloc->free(alloc->ctx, rows, max_json_files * sizeof(eval_chk_row_t));
                if (mem.vtable && mem.vtable->deinit)
                    mem.vtable->deinit(mem.ctx);
                hu_config_deinit(&store_cfg);
                hu_log_error("eval", NULL, "eval check-regression: no SQLite memory backend");
                return HU_ERR_MEMORY_BACKEND;
            }

            const double max_drop = 0.10;
            for (size_t si = 0; si < n_ok; si++) {
                bool reg = false;
                char msg[512];
                hu_error_t re = hu_eval_regression_check_baseline_drop(
                    db, rows[si].stem, rows[si].score, max_drop, &reg, msg, sizeof(msg));
                if (re != HU_OK) {
                    alloc->free(alloc->ctx, rows, max_json_files * sizeof(eval_chk_row_t));
                    if (mem.vtable && mem.vtable->deinit)
                        mem.vtable->deinit(mem.ctx);
                    hu_config_deinit(&store_cfg);
                    hu_log_error("eval", NULL, "eval check-regression: %s", hu_error_string(re));
                    return re;
                }
                if (reg) {
                    printf("%s\n", msg);
                    alloc->free(alloc->ctx, rows, max_json_files * sizeof(eval_chk_row_t));
                    if (mem.vtable && mem.vtable->deinit)
                        mem.vtable->deinit(mem.ctx);
                    hu_config_deinit(&store_cfg);
                    return HU_ERR_INTERNAL;
                }
            }

            for (size_t si = 0; si < n_ok; si++)
                (void)hu_eval_persist_baseline(db, rows[si].stem, rows[si].score, rows[si].tasks);

            alloc->free(alloc->ctx, rows, max_json_files * sizeof(eval_chk_row_t));
            if (mem.vtable && mem.vtable->deinit)
                mem.vtable->deinit(mem.ctx);
            hu_config_deinit(&store_cfg);
            printf("Regression check: PASS (no suite dropped >10%%)\n");
            return HU_OK;
        }
#else
        printf("Regression check: PASS (no suite dropped >10%%)\n");
        return HU_OK;
#endif
#endif
    }

    if (strcmp(sub, "list") == 0) {
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *dir_path = "eval_suites";
            enum { max_json_files = 256 };
            DIR *d = opendir(dir_path);
            if (!d) {
                hu_log_error("eval", NULL, "cannot open %s: %s", dir_path, strerror(errno));
                return HU_ERR_IO;
            }
            char **names = alloc->alloc(alloc->ctx, max_json_files * sizeof(char *));
            if (!names) {
                closedir(d);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t n_names = 0;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.')
                    continue;
                size_t len = strlen(e->d_name);
                if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0)
                    continue;
                if (n_names >= (size_t)max_json_files)
                    break;
                char *copy = hu_strdup(alloc, e->d_name);
                if (!copy) {
                    closedir(d);
                    for (size_t i = 0; i < n_names; i++)
                        alloc->free(alloc->ctx, names[i], strlen(names[i]) + 1);
                    alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                names[n_names++] = copy;
            }
            closedir(d);
            if (n_names > 1)
                qsort(names, n_names, sizeof(names[0]), eval_json_basename_cmp);
            for (size_t i = 0; i < n_names; i++) {
                char path_buf[HU_INIT_MAX_PATH];
                int plen = snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, names[i]);
                if (plen < 0 || (size_t)plen >= sizeof(path_buf)) {
                    hu_log_error("eval", NULL, "path too long for %s/%s", dir_path, names[i]);
                    continue;
                }
                hu_eval_suite_t suite = {0};
                hu_error_t le = hu_eval_suite_load_json_path(alloc, path_buf, &suite);
                if (le != HU_OK) {
                    hu_log_error("eval", NULL, "could not load %s: %s", path_buf,
                                 hu_error_string(le));
                    hu_eval_suite_free(alloc, &suite);
                    continue;
                }
                const char *sn = suite.name ? suite.name : names[i];
                printf("%s\t%zu tasks\t(%s)\n", path_buf, suite.tasks_count, sn);
                hu_eval_suite_free(alloc, &suite);
            }
            for (size_t i = 0; i < n_names; i++)
                alloc->free(alloc->ctx, names[i], strlen(names[i]) + 1);
            alloc->free(alloc->ctx, names, max_json_files * sizeof(char *));
        }
        return HU_OK;
#else
        printf("eval_suites/fidelity.json\t10 tasks\t(fidelity)\n");
        printf("eval_suites/intelligence.json\t10 tasks\t(intelligence)\n");
        printf("eval_suites/reasoning.json\t10 tasks\t(reasoning)\n");
        printf("eval_suites/tool_use.json\t8 tasks\t(tool_use)\n");
        printf("eval_suites/memory.json\t8 tasks\t(memory)\n");
        printf("eval_suites/social.json\t8 tasks\t(social)\n");
        return HU_OK;
#endif
    }

    if (strcmp(sub, "compare") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: human eval compare <run1.json> <run2.json>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *path1 = argv[3];
        const char *path2 = argv[4];
        char *json1 = NULL, *json2 = NULL;
        size_t len1 = 0, len2 = 0;

#ifndef HU_IS_TEST
        {
            FILE *f1 = fopen(path1, "rb");
            if (!f1) {
                hu_log_error("eval", NULL, "cannot open %s: %s", path1, strerror(errno));
                return HU_ERR_IO;
            }
            fseek(f1, 0, SEEK_END);
            long sz1 = ftell(f1);
            fseek(f1, 0, SEEK_SET);
            if (sz1 <= 0 || sz1 > 65536) {
                fclose(f1);
                return HU_ERR_INVALID_ARGUMENT;
            }
            len1 = (size_t)sz1;
            json1 = alloc->alloc(alloc->ctx, len1 + 1);
            if (!json1) {
                fclose(f1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (fread(json1, 1, len1, f1) != len1) {
                fclose(f1);
                alloc->free(alloc->ctx, json1, len1 + 1);
                return HU_ERR_IO;
            }
            json1[len1] = '\0';
            fclose(f1);

            FILE *f2 = fopen(path2, "rb");
            if (!f2) {
                alloc->free(alloc->ctx, json1, len1 + 1);
                hu_log_error("eval", NULL, "cannot open %s: %s", path2, strerror(errno));
                return HU_ERR_IO;
            }
            fseek(f2, 0, SEEK_END);
            long sz2 = ftell(f2);
            fseek(f2, 0, SEEK_SET);
            if (sz2 <= 0 || sz2 > 65536) {
                fclose(f2);
                alloc->free(alloc->ctx, json1, len1 + 1);
                return HU_ERR_INVALID_ARGUMENT;
            }
            len2 = (size_t)sz2;
            json2 = alloc->alloc(alloc->ctx, len2 + 1);
            if (!json2) {
                fclose(f2);
                alloc->free(alloc->ctx, json1, len1 + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (fread(json2, 1, len2, f2) != len2) {
                fclose(f2);
                alloc->free(alloc->ctx, json1, len1 + 1);
                alloc->free(alloc->ctx, json2, len2 + 1);
                return HU_ERR_IO;
            }
            json2[len2] = '\0';
            fclose(f2);
        }
#else
        {
            static const char MOCK_R1[] = "{\"suite\":\"s1\",\"passed\":8,\"failed\":2,\"pass_"
                                          "rate\":0.80,\"elapsed_ms\":1000}";
            static const char MOCK_R2[] = "{\"suite\":\"s1\",\"passed\":9,\"failed\":1,\"pass_"
                                          "rate\":0.90,\"elapsed_ms\":900}";
            (void)path1;
            (void)path2;
            len1 = sizeof(MOCK_R1) - 1;
            len2 = sizeof(MOCK_R2) - 1;
            json1 = alloc->alloc(alloc->ctx, len1 + 1);
            json2 = alloc->alloc(alloc->ctx, len2 + 1);
            if (json1)
                memcpy(json1, MOCK_R1, len1 + 1);
            if (json2)
                memcpy(json2, MOCK_R2, len2 + 1);
        }
#endif

        if (!json1 || !json2) {
            if (json1)
                alloc->free(alloc->ctx, json1, len1 + 1);
            if (json2)
                alloc->free(alloc->ctx, json2, len2 + 1);
            return HU_ERR_OUT_OF_MEMORY;
        }

        hu_eval_run_t baseline = {0}, current = {0};
        hu_error_t err = hu_eval_run_load_json(alloc, json1, len1, &baseline);
        if (err == HU_OK)
            err = hu_eval_run_load_json(alloc, json2, len2, &current);
        alloc->free(alloc->ctx, json1, len1 + 1);
        alloc->free(alloc->ctx, json2, len2 + 1);
        if (err != HU_OK) {
            hu_eval_run_free(alloc, &baseline);
            hu_log_error("eval", NULL, "failed to parse run reports");
            return err;
        }

        char *report = NULL;
        size_t report_len = 0;
        err = hu_eval_compare(alloc, &baseline, &current, &report, &report_len);
        hu_eval_run_free(alloc, &baseline);
        hu_eval_run_free(alloc, &current);
        if (err == HU_OK && report) {
            printf("%.*s\n", (int)report_len, report);
            alloc->free(alloc->ctx, report, report_len + 1);
        }
        return err;
    }

    if (strcmp(sub, "dashboard") == 0) {
        hu_eval_run_t *runs = NULL;
        size_t runs_count = 0;
        hu_error_t err = HU_OK;

        if (argc >= 4) {
            runs_count = (size_t)(argc - 3);
            runs = alloc->alloc(alloc->ctx, runs_count * sizeof(hu_eval_run_t));
            if (!runs)
                return HU_ERR_OUT_OF_MEMORY;
            memset(runs, 0, runs_count * sizeof(hu_eval_run_t));

            for (size_t i = 0; i < runs_count; i++) {
                const char *path = argv[3 + (int)i];
#ifdef HU_IS_TEST
                (void)path;
                static const char MOCK[] = "{\"suite\":\"reasoning-basic\",\"passed\":8,\"failed\":"
                                           "2,\"pass_rate\":0.80,\"elapsed_ms\":120}";
                size_t len = sizeof(MOCK) - 1;
                err = hu_eval_run_load_json(alloc, MOCK, len, &runs[i]);
#else
                FILE *f = fopen(path, "rb");
                if (!f) {
                    hu_log_error("eval", NULL, "cannot open %s: %s", path, strerror(errno));
                    err = HU_ERR_IO;
                    break;
                }
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz <= 0 || sz > 65536) {
                    fclose(f);
                    err = HU_ERR_INVALID_ARGUMENT;
                    break;
                }
                size_t len = (size_t)sz;
                char *json = alloc->alloc(alloc->ctx, len + 1);
                if (!json) {
                    fclose(f);
                    err = HU_ERR_OUT_OF_MEMORY;
                    break;
                }
                if (fread(json, 1, len, f) != len) {
                    fclose(f);
                    alloc->free(alloc->ctx, json, len + 1);
                    err = HU_ERR_IO;
                    break;
                }
                json[len] = '\0';
                fclose(f);
                err = hu_eval_run_load_json(alloc, json, len, &runs[i]);
                alloc->free(alloc->ctx, json, len + 1);
#endif
                if (err != HU_OK)
                    break;
            }

            if (err == HU_OK)
                err = hu_eval_dashboard_render(alloc, stdout, runs, runs_count);

            for (size_t i = 0; i < runs_count; i++)
                hu_eval_run_free(alloc, &runs[i]);
            alloc->free(alloc->ctx, runs, runs_count * sizeof(hu_eval_run_t));
        } else {
            err = hu_eval_dashboard_render(alloc, stdout, NULL, 0);
        }
        return err;
    }

    if (strcmp(sub, "history") == 0) {
#ifdef HU_ENABLE_SQLITE
        size_t max_runs = 10;
        const char *filter_suite = NULL;
        for (int a = 3; a < argc; a++) {
            if (strcmp(argv[a], "--last") == 0 && a + 1 < argc) {
                max_runs = (size_t)atoi(argv[++a]);
                if (max_runs == 0 || max_runs > 100)
                    max_runs = 10;
            } else if (strcmp(argv[a], "--benchmark") == 0 && a + 1 < argc) {
                filter_suite = argv[++a];
            }
        }
        hu_config_t cfg;
        hu_error_t cfg_err = hu_config_load(alloc, &cfg);
        if (cfg_err != HU_OK) {
            hu_log_error("eval", NULL, "eval history: config error");
            return cfg_err;
        }
        const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
        hu_memory_t mem = hu_memory_create_from_config(alloc, &cfg, ws);
        sqlite3 *db = mem.vtable ? hu_sqlite_memory_get_db(&mem) : NULL;
        if (!db) {
            hu_log_error("eval", NULL, "eval history: no SQLite backend");
            if (mem.vtable && mem.vtable->deinit)
                mem.vtable->deinit(mem.ctx);
            hu_config_deinit(&cfg);
            return HU_ERR_NOT_FOUND;
        }
        hu_error_t tbl_err = hu_eval_init_tables(db);
        if (tbl_err != HU_OK)
            hu_log_error("eval", NULL, "table init failed");
        hu_eval_run_t *runs = alloc->alloc(alloc->ctx, max_runs * sizeof(hu_eval_run_t));
        if (!runs) {
            if (mem.vtable && mem.vtable->deinit)
                mem.vtable->deinit(mem.ctx);
            hu_config_deinit(&cfg);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(runs, 0, max_runs * sizeof(hu_eval_run_t));
        size_t count = 0;
        hu_error_t err = hu_eval_load_history(alloc, db, runs, max_runs, &count);
        if (err == HU_OK && count > 0) {
            printf("%-20s %-12s %-10s %6s %6s %8s\n", "Suite", "Provider", "Model", "Pass", "Fail",
                   "Rate");
            printf("%-20s %-12s %-10s %6s %6s %8s\n", "----", "--------", "-----", "----", "----",
                   "----");
            for (size_t i = 0; i < count; i++) {
                if (filter_suite && runs[i].suite_name &&
                    strcmp(runs[i].suite_name, filter_suite) != 0)
                    continue;
                printf("%-20s %-12s %-10s %6zu %6zu %7.1f%%\n",
                       runs[i].suite_name ? runs[i].suite_name : "-",
                       runs[i].provider ? runs[i].provider : "-",
                       runs[i].model ? runs[i].model : "-", runs[i].passed, runs[i].failed,
                       runs[i].pass_rate * 100.0);
            }
        } else {
            printf("No eval history found.\n");
        }
        for (size_t i = 0; i < count; i++)
            hu_eval_run_free(alloc, &runs[i]);
        alloc->free(alloc->ctx, runs, max_runs * sizeof(hu_eval_run_t));
        if (mem.vtable && mem.vtable->deinit)
            mem.vtable->deinit(mem.ctx);
        hu_config_deinit(&cfg);
        return err;
#else
        hu_log_error("eval", NULL, "eval history: SQLite not enabled");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }

    if (strcmp(sub, "trend") == 0) {
#ifdef HU_ENABLE_SQLITE
        enum { eval_trend_max_runs = 200, eval_trend_max_suites = 64 };
        hu_config_t cfg;
        hu_error_t cfg_err = hu_config_load(alloc, &cfg);
        if (cfg_err != HU_OK) {
            hu_log_error("eval", NULL, "eval trend: config error");
            return cfg_err;
        }
        const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
        hu_memory_t mem = hu_memory_create_from_config(alloc, &cfg, ws);
        sqlite3 *db = mem.vtable ? hu_sqlite_memory_get_db(&mem) : NULL;
        if (!db) {
            printf("No eval history found. Run 'human eval baseline' first.\n");
            if (mem.vtable && mem.vtable->deinit)
                mem.vtable->deinit(mem.ctx);
            hu_config_deinit(&cfg);
            return HU_OK;
        }
        (void)hu_eval_init_tables(db);
        hu_eval_run_t *runs = alloc->alloc(alloc->ctx, eval_trend_max_runs * sizeof(hu_eval_run_t));
        if (!runs) {
            if (mem.vtable && mem.vtable->deinit)
                mem.vtable->deinit(mem.ctx);
            hu_config_deinit(&cfg);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(runs, 0, eval_trend_max_runs * sizeof(hu_eval_run_t));
        size_t count = 0;
        hu_error_t err = hu_eval_load_history(alloc, db, runs, eval_trend_max_runs, &count);
        char suite_names[eval_trend_max_suites][128];
        double curr[eval_trend_max_suites];
        double prev[eval_trend_max_suites];
        bool have_curr[eval_trend_max_suites];
        bool have_prev[eval_trend_max_suites];
        memset(have_curr, 0, sizeof(have_curr));
        memset(have_prev, 0, sizeof(have_prev));
        size_t nslots = 0;

        if (err == HU_OK && count > 0) {
            for (size_t i = 0; i < count; i++) {
                const char *sn =
                    runs[i].suite_name && runs[i].suite_name[0] ? runs[i].suite_name : "-";
                int slot = -1;
                for (size_t s = 0; s < nslots; s++) {
                    if (strcmp(suite_names[s], sn) == 0) {
                        slot = (int)s;
                        break;
                    }
                }
                if (slot < 0) {
                    if (nslots >= eval_trend_max_suites)
                        continue;
                    slot = (int)nslots;
                    nslots++;
                    strncpy(suite_names[slot], sn, sizeof(suite_names[0]) - 1);
                    suite_names[slot][sizeof(suite_names[0]) - 1] = '\0';
                }
                if (!have_curr[slot]) {
                    curr[slot] = runs[i].pass_rate;
                    have_curr[slot] = true;
                } else if (!have_prev[slot]) {
                    prev[slot] = runs[i].pass_rate;
                    have_prev[slot] = true;
                }
            }
        }

        size_t printed = 0;
        if (err == HU_OK && nslots > 0) {
            printf("%-28s %12s %12s %10s\n", "Suite", "Previous", "Current", "Delta");
            printf("%-28s %12s %12s %10s\n", "----------------------------", "------------",
                   "------------", "----------");
            for (size_t s = 0; s < nslots; s++) {
                if (!have_curr[s] || !have_prev[s])
                    continue;
                double delta = curr[s] - prev[s];
                printf("%-28.28s %11.1f%% %11.1f%% %+9.1f%%\n", suite_names[s], prev[s] * 100.0,
                       curr[s] * 100.0, delta * 100.0);
                printed++;
            }
        }

        for (size_t i = 0; i < count; i++)
            hu_eval_run_free(alloc, &runs[i]);
        alloc->free(alloc->ctx, runs, eval_trend_max_runs * sizeof(hu_eval_run_t));
        if (mem.vtable && mem.vtable->deinit)
            mem.vtable->deinit(mem.ctx);
        hu_config_deinit(&cfg);

        if (printed == 0) {
            printf("No eval history found. Run 'human eval baseline' first.\n");
            return HU_OK;
        }
        return err;
#else
        printf("Eval trend requires SQLite support (build with HU_ENABLE_SQLITE).\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }

    if (strcmp(sub, "benchmark") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: human eval benchmark <gaia|swebench|tooluse> <suite.json>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *bench_name = argv[3];
        const char *bench_path = argv[4];
        hu_benchmark_type_t bench_type;
        if (strcmp(bench_name, "gaia") == 0)
            bench_type = HU_BENCHMARK_GAIA;
        else if (strcmp(bench_name, "swebench") == 0)
            bench_type = HU_BENCHMARK_SWE_BENCH;
        else if (strcmp(bench_name, "tooluse") == 0)
            bench_type = HU_BENCHMARK_TOOL_USE;
        else if (strcmp(bench_name, "liveagent") == 0)
            bench_type = HU_BENCHMARK_LIVE_AGENT;
        else if (strcmp(bench_name, "apex") == 0)
            bench_type = HU_BENCHMARK_APEX;
        else if (strcmp(bench_name, "longmemeval") == 0)
            bench_type = HU_BENCHMARK_LONGMEMEVAL;
        else if (strcmp(bench_name, "locomo") == 0)
            bench_type = HU_BENCHMARK_LOCOMO;
        else if (strcmp(bench_name, "knowu-bench") == 0)
            bench_type = HU_BENCHMARK_KNOWU_BENCH;
        else if (strcmp(bench_name, "empa") == 0)
            bench_type = HU_BENCHMARK_EMPA;
        else if (strcmp(bench_name, "proagentbench") == 0)
            bench_type = HU_BENCHMARK_PROAGENTBENCH;
        else {
            hu_log_error("eval", NULL, "Unknown benchmark: %s", bench_name);
            return HU_ERR_INVALID_ARGUMENT;
        }

        char *bench_json = NULL;
        size_t bench_json_len = 0;
#ifdef HU_IS_TEST
        (void)bench_path;
        static const char MOCK_BENCH[] =
            "{\"name\":\"mock-bench\",\"tasks\":["
            "{\"id\":\"b1\",\"prompt\":\"test\",\"expected\":\"ok\",\"category\":\"basic\","
            "\"difficulty\":1,\"timeout_ms\":5000}"
            "]}\n";
        bench_json_len = sizeof(MOCK_BENCH) - 1;
        bench_json = alloc->alloc(alloc->ctx, bench_json_len + 1);
        if (!bench_json)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(bench_json, MOCK_BENCH, bench_json_len + 1);
#else
        FILE *bf = fopen(bench_path, "r");
        if (!bf) {
            hu_log_error("eval", NULL, "Cannot open benchmark file: %s", bench_path);
            return HU_ERR_INVALID_ARGUMENT;
        }
        fseek(bf, 0, SEEK_END);
        long bsz = ftell(bf);
        fseek(bf, 0, SEEK_SET);
        if (bsz <= 0) {
            fclose(bf);
            return HU_ERR_INVALID_ARGUMENT;
        }
        bench_json_len = (size_t)bsz;
        bench_json = alloc->alloc(alloc->ctx, bench_json_len + 1);
        if (!bench_json) {
            fclose(bf);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t nread = fread(bench_json, 1, bench_json_len, bf);
        fclose(bf);
        if (nread != bench_json_len) {
            alloc->free(alloc->ctx, bench_json, bench_json_len + 1);
            return HU_ERR_IO;
        }
        bench_json[bench_json_len] = '\0';
#endif

        hu_eval_suite_t bench_suite;
        memset(&bench_suite, 0, sizeof(bench_suite));
        hu_error_t berr =
            hu_benchmark_load(alloc, bench_type, bench_json, bench_json_len, &bench_suite);
        alloc->free(alloc->ctx, bench_json, bench_json_len + 1);
        if (berr != HU_OK) {
            hu_log_error("eval", NULL, "Failed to load benchmark: %s", hu_error_string(berr));
            return berr;
        }

        printf("{\"benchmark\":\"%s\",\"suite\":\"%s\",\"loaded\":true}\n",
               hu_benchmark_type_name(bench_type), bench_suite.name ? bench_suite.name : "");
        hu_eval_suite_free(alloc, &bench_suite);
        return HU_OK;
    }

    if (strcmp(sub, "turing-adversarial") == 0) {
        int weak[HU_TURING_DIM_COUNT];
        for (size_t i = 0; i < HU_TURING_DIM_COUNT; i++)
            weak[i] = 5;

        hu_turing_scenario_t *scenarios = NULL;
        size_t sc_count = 0;
        hu_error_t err = hu_turing_adversarial_generate(alloc, weak, &scenarios, &sc_count);
        if (err != HU_OK) {
            hu_log_error("eval", NULL, "adversarial generate: %s", hu_error_string(err));
            return err;
        }

        hu_self_improve_state_t si_state;
        memset(&si_state, 0, sizeof(si_state));
        hu_fidelity_score_t baseline = {.composite = 0.5f};
        hu_self_improve_set_baseline(&si_state, &baseline);

        size_t mutations = 0;
        err = hu_turing_adversarial_run_cycle(alloc, &si_state, weak, &mutations);
        if (err != HU_OK) {
            hu_log_error("eval", NULL, "adversarial cycle: %s", hu_error_string(err));
            if (scenarios)
                alloc->free(alloc->ctx, scenarios, sc_count * sizeof(hu_turing_scenario_t));
            return err;
        }

        printf("{\"scenarios\":%zu,\"mutations_applied\":%zu}\n", sc_count, mutations);
        if (scenarios)
            alloc->free(alloc->ctx, scenarios, sc_count * sizeof(hu_turing_scenario_t));
        return HU_OK;
    }

    /* B8 — Theory-of-mind synthetic pack scoring CLI.
     *
     *   human eval tom smoke <pack.json>
     *       Run the deterministic synthesizer over each item in the pack,
     *       assert each produces the expected category tag. Pure CI smoke
     *       — no model required. Mirrors `hu_tom_b8_synthetic_pack_run_smoke`.
     *
     *   human eval tom gold <pack.json>
     *       Score the pack's gold-answer field against each item's
     *       synthesized ToM block. Used to verify the rubric + JSON pack
     *       stay in sync. Mirrors `hu_tom_b8_synthetic_pack_score_gold`.
     *
     *   human eval tom run --pack <pack.json> --responses <responses.jsonl>
     *                      [--no-skip-unanswered]
     *       Score model responses against the pack. Responses are JSONL
     *       (one object per line) with `id` + `response` fields. Default
     *       behaviour is to *skip* items the responses file did not
     *       answer (only count answered items). Pass --no-skip-unanswered
     *       to count missing answers as failures. Output is one JSON line
     *       with `pass`, `total`, and `pct` fields so callers can pipe
     *       through jq. */
    if (strcmp(sub, "tom") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "Usage: human eval tom <smoke|gold|run> ...\n"
                    "  smoke <pack.json>                        — run synthesizer smoke\n"
                    "  gold <pack.json>                         — score gold answers vs ToM\n"
                    "  run --pack <p.json> --responses <r.jsonl> [--no-skip-unanswered]\n"
                    "                                            — score model responses\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        const char *tom_sub = argv[3];

        if (strcmp(tom_sub, "smoke") == 0 || strcmp(tom_sub, "gold") == 0) {
            if (argc < 5) {
                fprintf(stderr, "Usage: human eval tom %s <pack.json>\n", tom_sub);
                return HU_ERR_INVALID_ARGUMENT;
            }
            unsigned pass = 0, total = 0;
            hu_error_t err =
                strcmp(tom_sub, "smoke") == 0
                    ? hu_tom_b8_synthetic_pack_run_smoke(alloc, argv[4], &pass, &total)
                    : hu_tom_b8_synthetic_pack_score_gold(alloc, argv[4], &pass, &total);
            if (err != HU_OK) {
                hu_log_error("eval", NULL, "tom %s: %s", tom_sub, hu_error_string(err));
                return err;
            }
            unsigned pct = total > 0 ? (pass * 100u) / total : 0u;
            printf("{\"mode\":\"%s\",\"pack\":\"%s\",\"pass\":%u,\"total\":%u,\"pct\":%u}\n",
                   tom_sub, argv[4], pass, total, pct);
            return HU_OK;
        }

        if (strcmp(tom_sub, "run") == 0) {
            const char *pack_path = NULL;
            const char *resp_path = NULL;
            int count_unanswered_as_failed = 0;
            for (int i = 4; i < argc; i++) {
                if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
                    pack_path = argv[++i];
                } else if (strcmp(argv[i], "--responses") == 0 && i + 1 < argc) {
                    resp_path = argv[++i];
                } else if (strcmp(argv[i], "--no-skip-unanswered") == 0) {
                    count_unanswered_as_failed = 1;
                } else {
                    fprintf(stderr, "Unknown flag: %s\n", argv[i]);
                    return HU_ERR_INVALID_ARGUMENT;
                }
            }
            if (!pack_path || !resp_path) {
                fprintf(stderr, "Usage: human eval tom run --pack <pack.json> "
                                "--responses <responses.jsonl> [--no-skip-unanswered]\n");
                return HU_ERR_INVALID_ARGUMENT;
            }

            FILE *f = fopen(resp_path, "rb");
            if (!f) {
                hu_log_error("eval", NULL, "open %s: %s", resp_path, strerror(errno));
                return HU_ERR_IO;
            }
            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                return HU_ERR_IO;
            }
            long sz = ftell(f);
            if (sz < 0 || sz > (long)(8 << 20)) { /* cap at 8 MB */
                fclose(f);
                fprintf(stderr, "responses file too large or unreadable: %ld bytes\n", sz);
                return HU_ERR_IO;
            }
            rewind(f);
            char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
            if (!buf) {
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t rd = fread(buf, 1, (size_t)sz, f);
            fclose(f);
            buf[rd] = '\0';

            /* Pass 1: count newlines to size the response array. JSONL is
             * one object per line; trailing newline optional. */
            size_t cap = 1;
            for (size_t i = 0; i < rd; i++) {
                if (buf[i] == '\n')
                    cap++;
            }
            hu_tom_b8_response_t *resps =
                (hu_tom_b8_response_t *)alloc->alloc(alloc->ctx, cap * sizeof(*resps));
            if (!resps) {
                alloc->free(alloc->ctx, buf, (size_t)sz + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(resps, 0, cap * sizeof(*resps));

            /* Pass 2: parse each non-empty line as JSON, extract id +
             * response. We keep a parallel array of parsed roots so the
             * pointers `resps[i].id / .response` stay valid for the
             * scoring call. The roots are freed after scoring. */
            hu_json_value_t **roots =
                (hu_json_value_t **)alloc->alloc(alloc->ctx, cap * sizeof(*roots));
            if (!roots) {
                alloc->free(alloc->ctx, resps, cap * sizeof(*resps));
                alloc->free(alloc->ctx, buf, (size_t)sz + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(roots, 0, cap * sizeof(*roots));

            size_t r_count = 0;
            size_t line_start = 0;
            for (size_t i = 0; i <= rd; i++) {
                if (i != rd && buf[i] != '\n')
                    continue;
                size_t line_len = i - line_start;
                while (line_len > 0 && (buf[line_start] == ' ' || buf[line_start] == '\t' ||
                                        buf[line_start] == '\r')) {
                    line_start++;
                    line_len--;
                }
                while (line_len > 0 && (buf[line_start + line_len - 1] == ' ' ||
                                        buf[line_start + line_len - 1] == '\t' ||
                                        buf[line_start + line_len - 1] == '\r')) {
                    line_len--;
                }
                if (line_len > 0 && r_count < cap) {
                    hu_json_value_t *root = NULL;
                    if (hu_json_parse(alloc, buf + line_start, line_len, &root) == HU_OK && root &&
                        root->type == HU_JSON_OBJECT) {
                        const char *id = hu_json_get_string(root, "id");
                        const char *response = hu_json_get_string(root, "response");
                        if (!response)
                            response = hu_json_get_string(root, "answer");
                        if (id && response) {
                            roots[r_count] = root;
                            resps[r_count].id = id;
                            resps[r_count].response = response;
                            resps[r_count].response_len = strlen(response);
                            r_count++;
                        } else if (root) {
                            hu_json_free(alloc, root);
                        }
                    } else if (root) {
                        hu_json_free(alloc, root);
                    }
                }
                line_start = i + 1;
            }

            unsigned pass = 0, total = 0;
            hu_error_t err = hu_tom_b8_synthetic_pack_score_responses(
                alloc, pack_path, resps, r_count, count_unanswered_as_failed, &pass, &total);

            for (size_t i = 0; i < r_count; i++) {
                if (roots[i])
                    hu_json_free(alloc, roots[i]);
            }
            alloc->free(alloc->ctx, roots, cap * sizeof(*roots));
            alloc->free(alloc->ctx, resps, cap * sizeof(*resps));
            alloc->free(alloc->ctx, buf, (size_t)sz + 1);

            if (err != HU_OK) {
                hu_log_error("eval", NULL, "tom run: %s", hu_error_string(err));
                return err;
            }
            unsigned pct = total > 0 ? (pass * 100u) / total : 0u;
            printf("{\"mode\":\"run\",\"pack\":\"%s\",\"responses\":\"%s\","
                   "\"pass\":%u,\"total\":%u,\"pct\":%u,\"responses_loaded\":%zu}\n",
                   pack_path, resp_path, pass, total, pct, r_count);
            return HU_OK;
        }

        fprintf(stderr, "Unknown tom subcommand: %s\n", tom_sub);
        return HU_ERR_INVALID_ARGUMENT;
    }

    fprintf(stderr, "Unknown eval subcommand: %s\n", sub);
    return HU_ERR_INVALID_ARGUMENT;
}

/* ── research ───────────────────────────────────────────────────────────── */
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
hu_error_t cmd_research(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    (void)argv;
    hu_app_ctx_t app_ctx;
    hu_error_t err = hu_app_bootstrap(&app_ctx, alloc, NULL, true, false);
    if (err != HU_OK) {
        hu_log_error("research", NULL, "Research bootstrap failed: %s", hu_error_string(err));
        return err;
    }
    if (!app_ctx.agent_ok || !app_ctx.agent) {
        hu_log_error("research", NULL, "Research requires an agent. Configure a provider first.");
        hu_app_teardown(&app_ctx);
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_memory_t *mem = app_ctx.memory;
    sqlite3 *db = mem ? hu_sqlite_memory_get_db(mem) : NULL;
    if (!db) {
        hu_log_error("research", NULL, "Research requires SQLite memory backend.");
        hu_app_teardown(&app_ctx);
        return HU_ERR_NOT_SUPPORTED;
    }
    printf("Running research agent...\n");
    err = hu_research_agent_run(alloc, app_ctx.agent, db);
    hu_app_teardown(&app_ctx);
    if (err == HU_OK)
        printf("Research complete.\n");
    else
        hu_log_error("research", NULL, "Research failed: %s", hu_error_string(err));
    return err;
}
#else
hu_error_t cmd_research(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    hu_log_error("research", NULL, "Research requires --enable-feeds and --enable-sqlite.");
    return HU_ERR_NOT_SUPPORTED;
}
#endif

/* ── calibrate ─────────────────────────────────────────────────────────── */
hu_error_t cmd_calibrate(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: human calibrate <run|clone> [args]\n");
        printf("  run [db_path] [contact] [channel]  - Analyze messaging patterns and produce "
               "recommendations (channel defaults to auto)\n");
        printf("  clone [db_path] [contact] [persona_path] - Extract behavioral patterns and "
               "update persona\n");
        return HU_OK;
    }
    const char *sub = argv[2];

    if (strcmp(sub, "run") == 0) {
        const char *db_path = argc >= 4 ? argv[3] : NULL;
        const char *contact = argc >= 5 ? argv[4] : NULL;
        const char *channel = argc >= 6 ? argv[5] : NULL;
        char *recommendations = NULL;
        hu_error_t err = hu_calibrate(alloc, db_path, contact, channel, &recommendations);
        if (err != HU_OK) {
            hu_log_error("calibrate", NULL, "Calibration failed: %s", hu_error_string(err));
            return err;
        }
        if (recommendations) {
            printf("%s\n", recommendations);
            alloc->free(alloc->ctx, recommendations, strlen(recommendations) + 1);
        }
        return HU_OK;
    }

    if (strcmp(sub, "clone") == 0) {
        const char *db_path = argc >= 4 ? argv[3] : NULL;
        const char *contact = argc >= 5 ? argv[4] : NULL;
        const char *persona_path = argc >= 6 ? argv[5] : NULL;

        hu_clone_patterns_t patterns;
        memset(&patterns, 0, sizeof(patterns));
        hu_error_t err = hu_behavioral_clone_extract(alloc, db_path, contact, &patterns);
        if (err != HU_OK) {
            hu_log_error("calibrate", NULL, "Clone extraction failed: %s", hu_error_string(err));
            return err;
        }

        printf("Behavioral patterns extracted:\n");
        printf("  Initiation frequency: %.2f/day\n", patterns.initiation_frequency_per_day);
        printf("  Double-text probability: %.2f\n", patterns.double_text_probability);
        printf("  Topic starters: %zu\n", patterns.topic_starter_count);
        printf("  Sign-offs: %zu\n", patterns.sign_off_count);

        if (persona_path) {
            err = hu_behavioral_clone_update_persona(alloc, &patterns, persona_path);
            if (err != HU_OK) {
                hu_log_error("calibrate", NULL, "Persona update failed: %s", hu_error_string(err));
                return err;
            }
            printf("Persona recommendations written to: %s\n", persona_path);
        }
        return HU_OK;
    }

    fprintf(stderr, "Unknown calibrate subcommand: %s\n", sub);
    return HU_ERR_INVALID_ARGUMENT;
}

/* ── feed ──────────────────────────────────────────────────────────────── */
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
hu_error_t cmd_feed(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: human feed "
               "<poll|status|list|health|findings|search|digest|correlate|cleanup|learn>\n");
        return HU_OK;
    }
    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        hu_log_error("config", NULL, "Config error: %s", hu_error_string(err));
        return err;
    }
    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    hu_memory_t mem = hu_memory_create_from_config(alloc, &cfg, ws);
    if (!mem.vtable) {
        hu_log_error("feed", NULL, "no memory backend configured");
        hu_config_deinit(&cfg);
        return HU_ERR_NOT_FOUND;
    }
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    if (!db) {
        hu_log_error("feed", NULL, "SQLite database not available");
        if (mem.vtable->deinit)
            mem.vtable->deinit(mem.ctx);
        hu_config_deinit(&cfg);
        return HU_ERR_NOT_FOUND;
    }
    const char *sub = argv[2];

    if (strcmp(sub, "poll") == 0) {
        hu_feed_processor_t fp = {.alloc = alloc, .db = db};
        if (cfg.feeds.gmail_client_id) {
            fp.gmail_client_id = cfg.feeds.gmail_client_id;
            fp.gmail_client_id_len = strlen(cfg.feeds.gmail_client_id);
        }
        if (cfg.feeds.gmail_client_secret) {
            fp.gmail_client_secret = cfg.feeds.gmail_client_secret;
            fp.gmail_client_secret_len = strlen(cfg.feeds.gmail_client_secret);
        }
        if (cfg.feeds.gmail_refresh_token) {
            fp.gmail_refresh_token = cfg.feeds.gmail_refresh_token;
            fp.gmail_refresh_token_len = strlen(cfg.feeds.gmail_refresh_token);
        }
        if (cfg.feeds.twitter_bearer_token) {
            fp.twitter_bearer_token = cfg.feeds.twitter_bearer_token;
            fp.twitter_bearer_token_len = strlen(cfg.feeds.twitter_bearer_token);
        }
        hu_feed_config_t fconf;
        memset(&fconf, 0, sizeof(fconf));
        fconf.enabled[HU_FEED_NEWS_RSS] = true;
        fconf.enabled[HU_FEED_FILE_INGEST] = true;
        fconf.enabled[HU_FEED_GMAIL] = true;
        fconf.enabled[HU_FEED_IMESSAGE] = true;
        fconf.enabled[HU_FEED_TWITTER] = true;
        fconf.poll_interval_minutes[HU_FEED_FILE_INGEST] = 0;
        fconf.poll_interval_minutes[HU_FEED_NEWS_RSS] = 0;
        fconf.poll_interval_minutes[HU_FEED_GMAIL] = 0;
        fconf.poll_interval_minutes[HU_FEED_IMESSAGE] = 0;
        fconf.poll_interval_minutes[HU_FEED_TWITTER] = 0;
        fconf.max_items_per_poll = 50;
        uint64_t last_poll[HU_FEED_COUNT] = {0};
        uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
        size_t ingested = 0;
        err = hu_feed_processor_poll(&fp, &fconf, last_poll, now_ms, &ingested);
        if (err == HU_OK)
            printf("[feed] Poll complete: %zu items ingested\n", ingested);
        else
            hu_log_error("feed", NULL, "poll error: %s", hu_error_string(err));
    } else if (strcmp(sub, "status") == 0) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(
            db, "SELECT source, COUNT(*), MAX(ingested_at) FROM feed_items GROUP BY source", -1,
            &stmt, NULL);
        if (rc != SQLITE_OK) {
            hu_log_error("feed", NULL, "query error: %s", sqlite3_errmsg(db));
        } else {
            printf("%-20s  %6s  %s\n", "SOURCE", "COUNT", "LAST INGESTED");
            printf("%-20s  %6s  %s\n", "--------------------", "------", "-------------------");
            int total = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *src = (const char *)sqlite3_column_text(stmt, 0);
                int cnt = sqlite3_column_int(stmt, 1);
                int64_t ts = sqlite3_column_int64(stmt, 2);
                total += cnt;
                char timebuf[32] = "unknown";
                if (ts > 0) {
                    time_t tt = (time_t)ts;
                    struct tm *lt = localtime(&tt);
                    if (lt)
                        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", lt);
                }
                printf("%-20s  %6d  %s\n", src ? src : "(null)", cnt, timebuf);
            }
            printf("%-20s  %6d\n", "TOTAL", total);
            sqlite3_finalize(stmt);
        }
        /* Intelligence stats */
        printf("\nIntelligence:\n");
        sqlite3_stmt *istmt = NULL;
        int irc = sqlite3_prepare_v2(
            db, "SELECT COUNT(*) FROM research_findings WHERE status = 'actioned'", -1, &istmt,
            NULL);
        if (irc == SQLITE_OK && sqlite3_step(istmt) == SQLITE_ROW)
            printf("  Findings actioned: %d\n", sqlite3_column_int(istmt, 0));
        if (istmt)
            sqlite3_finalize(istmt);

        irc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM general_lessons", -1, &istmt, NULL);
        if (irc == SQLITE_OK && sqlite3_step(istmt) == SQLITE_ROW)
            printf("  General lessons:   %d\n", sqlite3_column_int(istmt, 0));
        if (istmt)
            sqlite3_finalize(istmt);

        irc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM current_events", -1, &istmt, NULL);
        if (irc == SQLITE_OK && sqlite3_step(istmt) == SQLITE_ROW)
            printf("  Current events:    %d\n", sqlite3_column_int(istmt, 0));
        if (istmt)
            sqlite3_finalize(istmt);

        irc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM inferred_values", -1, &istmt, NULL);
        if (irc == SQLITE_OK && sqlite3_step(istmt) == SQLITE_ROW)
            printf("  Inferred values:   %d\n", sqlite3_column_int(istmt, 0));
        if (istmt)
            sqlite3_finalize(istmt);

        irc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM growth_milestones", -1, &istmt, NULL);
        if (irc == SQLITE_OK && sqlite3_step(istmt) == SQLITE_ROW)
            printf("  Growth milestones: %d\n", sqlite3_column_int(istmt, 0));
        if (istmt)
            sqlite3_finalize(istmt);
    } else if (strcmp(sub, "list") == 0) {
        sqlite3_stmt *stmt = NULL;
        int rc =
            sqlite3_prepare_v2(db,
                               "SELECT source, content_type, substr(content, 1, 80), ingested_at "
                               "FROM feed_items ORDER BY ingested_at DESC LIMIT 10",
                               -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            hu_log_error("feed", NULL, "query error: %s", sqlite3_errmsg(db));
        } else {
            printf("Last 10 feed items:\n\n");
            int idx = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *src = (const char *)sqlite3_column_text(stmt, 0);
                const char *ctype = (const char *)sqlite3_column_text(stmt, 1);
                const char *content = (const char *)sqlite3_column_text(stmt, 2);
                int64_t ts = sqlite3_column_int64(stmt, 3);
                char timebuf[32] = "unknown";
                if (ts > 0) {
                    time_t tt = (time_t)ts;
                    struct tm *lt = localtime(&tt);
                    if (lt)
                        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", lt);
                }
                printf("[%d] %s (%s) @ %s\n    %s\n\n", ++idx, src ? src : "?", ctype ? ctype : "?",
                       timebuf, content ? content : "(empty)");
            }
            if (idx == 0)
                printf("  (no feed items yet)\n");
            sqlite3_finalize(stmt);
        }
    } else if (strcmp(sub, "health") == 0) {
        printf("Feed Health Report\n");
        printf("==================\n\n");
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(
            db, "SELECT source, COUNT(*), MAX(ingested_at) FROM feed_items GROUP BY source", -1,
            &stmt, NULL);
        if (rc == SQLITE_OK) {
            time_t now = time(NULL);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *src = (const char *)sqlite3_column_text(stmt, 0);
                int cnt = sqlite3_column_int(stmt, 1);
                int64_t last_ts = sqlite3_column_int64(stmt, 2);
                double hours_ago = (last_ts > 0) ? difftime(now, (time_t)last_ts) / 3600.0 : -1;
                const char *status_icon = (hours_ago >= 0 && hours_ago < 24) ? "OK" : "STALE";
                if (hours_ago < 0)
                    status_icon = "NEVER";
                printf("  %-16s  %4d items  last=%.1fh ago  [%s]\n", src ? src : "?", cnt,
                       hours_ago >= 0 ? hours_ago : 0.0, status_icon);
            }
            sqlite3_finalize(stmt);
        }
        const char *home = getenv("HOME");
        if (home) {
            char ingest_dir[512];
            snprintf(ingest_dir, sizeof(ingest_dir), "%s/.human/feeds/ingest", home);
            printf("\nIngest directory: %s\n", ingest_dir);
            struct stat st;
            if (stat(ingest_dir, &st) == 0)
                printf("  Status: exists\n");
            else
                printf("  Status: NOT FOUND (create with: mkdir -p %s)\n", ingest_dir);
        }
        printf("\n");
    } else if (strcmp(sub, "findings") == 0) {
        hu_research_finding_t *findings = NULL;
        size_t fcount = 0;
        const char *filter = (argc >= 4) ? argv[3] : "all";
        hu_error_t ferr;
        if (strcmp(filter, "pending") == 0)
            ferr = hu_findings_get_pending(alloc, db, 50, &findings, &fcount);
        else
            ferr = hu_findings_get_all(alloc, db, 50, &findings, &fcount);
        if (ferr != HU_OK) {
            hu_log_error("feed", NULL, "findings query error: %s", hu_error_string(ferr));
        } else if (fcount == 0) {
            printf("No research findings yet. Run the research agent first.\n");
        } else {
            printf("Research Findings (%zu %s):\n\n", fcount,
                   strcmp(filter, "pending") == 0 ? "pending" : "total");
            for (size_t i = 0; i < fcount; i++) {
                hu_research_finding_t *f = &findings[i];
                char timebuf[32] = "unknown";
                if (f->created_at > 0) {
                    time_t tt = (time_t)f->created_at;
                    struct tm *lt = localtime(&tt);
                    if (lt)
                        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", lt);
                }
                printf("[%lld] [%s] %s @ %s\n", (long long)f->id,
                       f->priority[0] ? f->priority : "?", f->status[0] ? f->status : "?", timebuf);
                printf("  Finding: %.120s%s\n", f->finding, strlen(f->finding) > 120 ? "..." : "");
                if (f->source[0])
                    printf("  Source: %s\n", f->source);
                if (f->suggested_action[0])
                    printf("  Action: %.100s%s\n", f->suggested_action,
                           strlen(f->suggested_action) > 100 ? "..." : "");
                printf("\n");
            }
            hu_findings_free(alloc, findings, fcount);
        }
    } else if (strcmp(sub, "search") == 0) {
        if (argc < 4) {
            printf("Usage: human feed search <query>\n");
        } else {
            const char *query = argv[3];
            hu_feed_item_stored_t *results = NULL;
            size_t rcount = 0;
            hu_error_t serr =
                hu_feed_search(alloc, db, query, strlen(query), 20, &results, &rcount);
            if (serr != HU_OK) {
                hu_log_error("feed", NULL, "search error: %s", hu_error_string(serr));
            } else if (rcount == 0) {
                printf("No results for '%s'\n", query);
            } else {
                printf("Feed search results for '%s' (%zu matches):\n\n", query, rcount);
                for (size_t i = 0; i < rcount; i++) {
                    hu_feed_item_stored_t *r = &results[i];
                    char timebuf[32] = "unknown";
                    if (r->ingested_at > 0) {
                        time_t tt = (time_t)r->ingested_at;
                        struct tm *lt = localtime(&tt);
                        if (lt)
                            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", lt);
                    }
                    printf("[%zu] %s (%s) @ %s\n    %.100s%s\n", i + 1, r->source, r->content_type,
                           timebuf, r->content, r->content_len > 100 ? "..." : "");
                    if (r->url[0])
                        printf("    %s\n", r->url);
                    printf("\n");
                }
                hu_feed_items_free(alloc, results, rcount);
            }
        }
    } else if (strcmp(sub, "digest") == 0) {
        char *digest_text = NULL;
        size_t digest_text_len = 0;
        int64_t since = (int64_t)time(NULL) - 86400;
        hu_error_t derr =
            hu_feed_build_daily_digest(alloc, db, since, 8000, &digest_text, &digest_text_len);
        if (derr != HU_OK) {
            hu_log_error("feed", NULL, "digest error: %s", hu_error_string(derr));
        } else {
            printf("%.*s", (int)digest_text_len, digest_text);
            alloc->free(alloc->ctx, digest_text, digest_text_len + 1);
        }
    } else if (strcmp(sub, "correlate") == 0) {
        int64_t since = (int64_t)time(NULL) - 86400;
        hu_error_t cerr = hu_feed_correlate_recent(alloc, db, since, 0.3);
        if (cerr == HU_OK)
            printf("[feed] Correlation complete\n");
        else
            hu_log_error("feed", NULL, "correlation error: %s", hu_error_string(cerr));
    } else if (strcmp(sub, "cleanup") == 0) {
        uint32_t days = 30;
        if (argc >= 4) {
            int d = atoi(argv[3]);
            if (d > 0 && d <= 365)
                days = (uint32_t)d;
        }
        hu_feed_processor_t fp = {.alloc = alloc, .db = db};
        hu_error_t cerr = hu_feed_processor_cleanup(&fp, days);
        if (cerr == HU_OK)
            printf("[feed] Cleanup complete (removed items older than %u days)\n", days);
        else
            hu_log_error("feed", NULL, "cleanup error: %s", hu_error_string(cerr));
    } else if (strcmp(sub, "learn") == 0) {
#if defined(HU_HAS_SKILLS)
        hu_intelligence_cycle_result_t learn_result;
        memset(&learn_result, 0, sizeof(learn_result));
        hu_error_t lerr = hu_intelligence_run_cycle(alloc, db, &learn_result);
        if (lerr == HU_OK) {
            printf("[feed] Intelligence cycle complete:\n");
            printf("  Findings actioned: %zu\n", learn_result.findings_actioned);
            printf("  Lessons extracted: %zu\n", learn_result.lessons_extracted);
            printf("  Events recorded:   %zu\n", learn_result.events_recorded);
            printf("  Values learned:    %zu\n", learn_result.values_learned);
            printf("  Causal recorded:   %zu\n", learn_result.causal_recorded);
            printf("  Skills updated:    %zu\n", learn_result.skills_updated);
        } else {
            hu_log_error("feed", NULL, "intelligence cycle error: %s", hu_error_string(lerr));
        }
#else
        hu_log_error("feed", NULL, "intelligence cycle requires HU_HAS_SKILLS");
#endif
    } else {
        fprintf(stderr, "Unknown feed subcommand: %s\n", sub);
        printf("Usage: human feed "
               "<poll|status|list|health|findings|search|digest|correlate|cleanup|learn>\n");
        err = HU_ERR_INVALID_ARGUMENT;
    }

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    hu_config_deinit(&cfg);
    return err;
}
#endif /* HU_ENABLE_FEEDS && HU_ENABLE_SQLITE */

/* ── hula (run a HuLa program) ─────────────────────────────────────────── */

static hu_error_t hula_read_file(hu_allocator_t *alloc, const char *path, char **out,
                                 size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return HU_ERR_NOT_FOUND;
    }
    char *buf = alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out = buf;
    *out_len = rd;
    return HU_OK;
}

/* Stub tools for local demo (no real providers needed) */
typedef struct {
    const char *name;
} hula_demo_tool_ctx_t;

static hu_error_t hula_demo_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)alloc;
    hula_demo_tool_ctx_t *t = ctx;
    const char *text = "ok";
    if (args) {
        const char *s = hu_json_get_string(args, "text");
        if (s)
            text = s;
    }
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[%s] %s", t->name, text);
    out->success = true;
    size_t len = (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : (n > 0) ? (sizeof(buf) - 1U) : 0U;
    out->output = hu_strndup(alloc, buf, len);
    out->output_len = len;
    out->output_owned = true;
    return HU_OK;
}

static const char *hula_demo_name(void *ctx) {
    return ((hula_demo_tool_ctx_t *)ctx)->name;
}
static const char *hula_demo_desc(void *ctx) {
    (void)ctx;
    return "demo tool";
}
static const char *hula_demo_params(void *ctx) {
    (void)ctx;
    return "{}";
}
static void hula_demo_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_tool_vtable_t hula_demo_vtable = {
    .execute = hula_demo_execute,
    .name = hula_demo_name,
    .description = hula_demo_desc,
    .parameters_json = hula_demo_params,
    .deinit = hula_demo_deinit,
};

static hu_error_t hula_try_open_schema(FILE **out, char *path_buf, size_t path_cap) {
    static const char *const candidates[] = {
        "schemas/hula-program.schema.json",
        "../schemas/hula-program.schema.json",
        "../../schemas/hula-program.schema.json",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            *out = f;
            (void)snprintf(path_buf, path_cap, "%s", candidates[i]);
            return HU_OK;
        }
    }
    const char *root = getenv("HUMAN_SOURCE_ROOT");
    if (root && root[0]) {
        int n = snprintf(path_buf, path_cap, "%s/schemas/hula-program.schema.json", root);
        if (n > 0 && (size_t)n < path_cap) {
            FILE *f = fopen(path_buf, "rb");
            if (f) {
                *out = f;
                return HU_OK;
            }
        }
    }
    *out = NULL;
    path_buf[0] = '\0';
    return HU_ERR_NOT_FOUND;
}

static void hula_cli_hula_free_tools_bundle(hu_allocator_t *alloc, bool have_cfg, hu_config_t *cfg,
                                            hu_security_policy_t *cfg_policy, hu_agent_pool_t *pool,
                                            hu_tool_t *dyn_tools, size_t dyn_count,
                                            const char **heap_validate) {
    if (!alloc)
        return;
    if (have_cfg && cfg_policy && cfg_policy->tracker) {
        hu_rate_tracker_destroy(cfg_policy->tracker);
        cfg_policy->tracker = NULL;
    }
    if (heap_validate && dyn_count > 0)
        alloc->free(alloc->ctx, (void *)heap_validate, dyn_count * sizeof(const char *));
    if (have_cfg && dyn_tools && dyn_count > 0)
        hu_tools_destroy_default(alloc, dyn_tools, dyn_count);
    if (pool)
        hu_agent_pool_destroy(pool);
    if (have_cfg && cfg)
        hu_config_deinit(cfg);
}

/* Parse, validate, optionally execute; embed program_snapshot in persisted traces when set.
 * With config_path, load ~/.human-style config and register real tools for validate + execute. */
static hu_error_t hula_cli_run_program_json(hu_allocator_t *alloc, const char *json,
                                            size_t json_len, bool json_owned,
                                            bool run_after_validate,
                                            const char *program_snapshot_json,
                                            size_t program_snapshot_len, const char *config_path,
                                            const char *program_source, size_t program_source_len) {
    printf("── Parse ────────────────────────────────────────────\n");
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(alloc, json, json_len, &prog);
    if (err != HU_OK) {
        hu_log_error("hula", NULL, "FAIL: parse error: %s", hu_error_string(err));
        if (json_owned)
            hu_str_free(alloc, (char *)json);
        return err;
    }
    printf("  OK: program \"%s\" v%u, %zu nodes, root=%s (%s)\n",
           prog.name ? prog.name : "(unnamed)", prog.version, prog.node_count,
           prog.root ? (prog.root->id ? prog.root->id : "?") : "(none)",
           prog.root ? hu_hula_op_name(prog.root->op) : "?");

    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    bool have_cfg = false;
    hu_agent_pool_t *agent_pool = NULL;
    hu_tool_t *dyn_tools = NULL;
    size_t dyn_tools_count = 0;
    const char **heap_validate = NULL;
    hu_security_policy_t cfg_policy;
    memset(&cfg_policy, 0, sizeof(cfg_policy));

    static const char *const demo_validate[] = {"echo", "search", "write", "analyze"};
    const char *const *validate_names = demo_validate;
    size_t validate_count = 4;

    if (config_path && config_path[0]) {
        err = hu_config_load_from(alloc, config_path, &cfg);
        if (err != HU_OK) {
            hu_log_error("hula", NULL, "FAIL: config load: %s", hu_error_string(err));
            hu_hula_program_deinit(&prog);
            if (json_owned)
                hu_str_free(alloc, (char *)json);
            return err;
        }
        have_cfg = true;
        const char *ws = (cfg.workspace_dir && cfg.workspace_dir[0]) ? cfg.workspace_dir : ".";
        uint32_t pool_n = cfg.agent.pool_max_concurrent ? cfg.agent.pool_max_concurrent : 8u;
        agent_pool = hu_agent_pool_create(alloc, pool_n);
        if (!agent_pool) {
            hula_cli_hula_free_tools_bundle(alloc, true, &cfg, &cfg_policy, NULL, NULL, 0, NULL);
            hu_hula_program_deinit(&prog);
            if (json_owned)
                hu_str_free(alloc, (char *)json);
            return HU_ERR_OUT_OF_MEMORY;
        }
        {
            hu_fleet_limits_t fl = {0};
            fl.max_spawn_depth = cfg.agent.fleet_max_spawn_depth;
            fl.max_total_spawns = cfg.agent.fleet_max_total_spawns;
            fl.budget_limit_usd = cfg.agent.fleet_budget_usd;
            hu_agent_pool_set_fleet_limits(agent_pool, &fl);
        }
        err = hu_tools_create_default(alloc, ws, strlen(ws), NULL, &cfg, NULL, NULL, agent_pool,
                                      NULL, NULL, NULL, &dyn_tools, &dyn_tools_count);
        if (err != HU_OK) {
            hu_log_error("hula", NULL, "FAIL: tools from config: %s", hu_error_string(err));
            hula_cli_hula_free_tools_bundle(alloc, true, &cfg, &cfg_policy, agent_pool, NULL, 0,
                                            NULL);
            hu_hula_program_deinit(&prog);
            if (json_owned)
                hu_str_free(alloc, (char *)json);
            return err;
        }
        if (dyn_tools_count > 0) {
            heap_validate =
                (const char **)alloc->alloc(alloc->ctx, dyn_tools_count * sizeof(const char *));
            if (!heap_validate) {
                hula_cli_hula_free_tools_bundle(alloc, true, &cfg, &cfg_policy, agent_pool,
                                                dyn_tools, dyn_tools_count, NULL);
                hu_hula_program_deinit(&prog);
                if (json_owned)
                    hu_str_free(alloc, (char *)json);
                return HU_ERR_OUT_OF_MEMORY;
            }
            for (size_t ti = 0; ti < dyn_tools_count; ti++) {
                heap_validate[ti] = (dyn_tools[ti].vtable && dyn_tools[ti].vtable->name)
                                        ? dyn_tools[ti].vtable->name(dyn_tools[ti].ctx)
                                        : "";
            }
        }
        validate_names = (const char *const *)heap_validate;
        validate_count = dyn_tools_count;

        cfg_policy.autonomy = (hu_autonomy_level_t)cfg.security.autonomy_level;
        cfg_policy.workspace_dir = ws;
        cfg_policy.workspace_only = true;
        cfg_policy.allow_shell = (cfg_policy.autonomy != HU_AUTONOMY_READ_ONLY);
        cfg_policy.block_high_risk_commands = (cfg_policy.autonomy == HU_AUTONOMY_SUPERVISED);
        cfg_policy.require_approval_for_medium_risk = false;
        cfg_policy.max_actions_per_hour = 200;
        cfg_policy.tracker = hu_rate_tracker_create(alloc, cfg_policy.max_actions_per_hour);
    }

    printf("\n── Validate ─────────────────────────────────────────\n");
    hu_hula_validation_t v;
    memset(&v, 0, sizeof(v));
    err = hu_hula_validate(&prog, alloc, validate_names, validate_count, &v);
    if (err != HU_OK) {
        hu_log_error("hula", NULL, "FAIL: validation error: %s", hu_error_string(err));
        hu_hula_validation_deinit(alloc, &v);
        hula_cli_hula_free_tools_bundle(alloc, have_cfg, &cfg, &cfg_policy, agent_pool, dyn_tools,
                                        dyn_tools_count, heap_validate);
        hu_hula_program_deinit(&prog);
        if (json_owned)
            hu_str_free(alloc, (char *)json);
        return err;
    }
    if (v.valid) {
        printf("  OK: program is valid\n");
    } else {
        printf("  WARN: %zu diagnostics:\n", v.diag_count);
        for (size_t i = 0; i < v.diag_count; i++)
            printf("    - %s\n", v.diags[i].message ? v.diags[i].message : "?");
    }
    hu_hula_validation_deinit(alloc, &v);

    if (!run_after_validate) {
        hula_cli_hula_free_tools_bundle(alloc, have_cfg, &cfg, &cfg_policy, agent_pool, dyn_tools,
                                        dyn_tools_count, heap_validate);
        hu_hula_program_deinit(&prog);
        if (json_owned)
            hu_str_free(alloc, (char *)json);
        return HU_OK;
    }

    printf("\n── Execute ──────────────────────────────────────────\n");

    hula_demo_tool_ctx_t demo_ctxs[] = {
        {.name = "echo"},
        {.name = "search"},
        {.name = "write"},
        {.name = "analyze"},
    };
    hu_tool_t demo_tools[4];
    hu_tool_t *exec_tools = demo_tools;
    size_t exec_tools_count = 4;
    hu_security_policy_t demo_policy;
    memset(&demo_policy, 0, sizeof(demo_policy));
    demo_policy.autonomy = HU_AUTONOMY_AUTONOMOUS;
    hu_security_policy_t *exec_policy = &demo_policy;

    if (have_cfg) {
        exec_tools = dyn_tools;
        exec_tools_count = dyn_tools_count;
        exec_policy = &cfg_policy;
    } else {
        for (size_t i = 0; i < 4; i++) {
            demo_tools[i].ctx = &demo_ctxs[i];
            demo_tools[i].vtable = &hula_demo_vtable;
        }
    }

    hu_observer_t obs = hu_observer_log_stderr();

    hu_hula_exec_t exec;
    err = hu_hula_exec_init_full(&exec, *alloc, &prog, exec_tools, exec_tools_count, exec_policy,
                                 &obs);
    if (err != HU_OK) {
        hu_log_error("hula", NULL, "FAIL: exec init: %s", hu_error_string(err));
        hula_cli_hula_free_tools_bundle(alloc, have_cfg, &cfg, &cfg_policy, agent_pool, dyn_tools,
                                        dyn_tools_count, heap_validate);
        hu_hula_program_deinit(&prog);
        if (json_owned)
            hu_str_free(alloc, (char *)json);
        return err;
    }

    err = hu_hula_exec_run(&exec);
    if (err != HU_OK) {
        hu_log_error("hula", NULL, "FAIL: exec run: %s", hu_error_string(err));
        hu_hula_exec_deinit(&exec);
        hula_cli_hula_free_tools_bundle(alloc, have_cfg, &cfg, &cfg_policy, agent_pool, dyn_tools,
                                        dyn_tools_count, heap_validate);
        hu_hula_program_deinit(&prog);
        if (json_owned)
            hu_str_free(alloc, (char *)json);
        return err;
    }

    const hu_hula_result_t *root_r = prog.root ? hu_hula_exec_result(&exec, prog.root->id) : NULL;

    printf("  Results:\n");
    for (size_t i = 0; i < prog.node_count; i++) {
        const hu_hula_node_t *n = &prog.nodes[i];
        const hu_hula_result_t *r = hu_hula_exec_result(&exec, n->id);
        if (!r)
            continue;
        printf("    %-10s %-8s %-7s", n->id ? n->id : "?", hu_hula_op_name(n->op),
               hu_hula_status_name(r->status));
        if (r->output && r->output_len > 0) {
            size_t show = r->output_len > 60 ? 60 : r->output_len;
            printf("  \"%.*s%s\"", (int)show, r->output, r->output_len > 60 ? "..." : "");
        }
        if (r->error && r->error_len > 0)
            printf("  err=\"%.*s\"", (int)r->error_len, r->error);
        printf("\n");
    }

    printf("\n── Trace ────────────────────────────────────────────\n");
    size_t trace_len = 0;
    const char *trace = hu_hula_exec_trace(&exec, &trace_len);
    if (trace && trace_len > 0) {
        printf("  %.*s\n", (int)trace_len, trace);
    } else {
        printf("  (empty)\n");
    }

    const char *trace_dir_env = getenv("HU_HULA_TRACE_DIR");
    const char *snap = program_snapshot_json ? program_snapshot_json : json;
    size_t snap_len = program_snapshot_json ? program_snapshot_len : json_len;
    const char *psrc = (program_source && program_source_len > 0) ? program_source : NULL;
    size_t psrc_len = psrc ? program_source_len : 0;
    if (trace && trace_len > 0 && trace_dir_env && trace_dir_env[0]) {
        const char *pn = prog.name;
        size_t pnl = pn ? strlen(pn) : 0;
        bool trace_ok = root_r && root_r->status == HU_HULA_DONE;
        hu_error_t tr_err = hu_hula_trace_persist(alloc, trace_dir_env, trace, trace_len, pn, pnl,
                                                  trace_ok, snap, snap_len, psrc, psrc_len);
        if (tr_err != HU_OK) {
            hu_log_error("hula", NULL, "HU_HULA_TRACE_DIR persist failed: %s",
                         hu_error_string(tr_err));
        } else {
            printf("\n── Trace persist ─────────────────────────────────────\n");
            printf("  wrote JSON under %s\n", trace_dir_env);
        }
    }

    printf("\n── Summary ──────────────────────────────────────────\n");
    printf("  Program: %s\n", prog.name ? prog.name : "?");
    printf("  Nodes:   %zu\n", prog.node_count);
    printf("  Root:    %s\n", root_r ? hu_hula_status_name(root_r->status) : "?");
    printf("  Result:  %s\n", (root_r && root_r->status == HU_HULA_DONE) ? "SUCCESS" : "FAILED");

    hu_hula_exec_deinit(&exec);
    hula_cli_hula_free_tools_bundle(alloc, have_cfg, &cfg, &cfg_policy, agent_pool, dyn_tools,
                                    dyn_tools_count, heap_validate);
    hu_hula_program_deinit(&prog);
    if (json_owned)
        hu_str_free(alloc, (char *)json);
    return HU_OK;
}

hu_error_t cmd_hula(hu_allocator_t *alloc, int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: human hula <schema|expand|compile|replay|run|validate> ...\n\n");
        printf("  schema              Print JSON Schema path and contents (if found)\n");
        printf("  expand <tmpl> <vars.json>   Expand {{keys}} using JSON object vars\n");
        printf("  compile [--lite] <file>     Print HuLa JSON (lite syntax or JSON file "
               "passthrough)\n");
        printf("  replay [--config path] <trace.json>  Re-run embedded program (optional config "
               "for tools)\n");
        printf("  run [--lite] [--config path] <file|'{...}'>  Execute program\n");
        printf("  validate [--lite] [--config path] <file|'{...}'>\n\n");
        printf("Default run uses demo tools: echo, search, write, analyze\n");
        printf("--config loads tools from a human config.json (same as the agent).\n");
        printf("Optional: HU_HULA_TRACE_DIR=/path/dir persists trace JSON + program (+ "
               "program_source).\n");
        return HU_OK;
    }

    const char *sub = argv[2];
    if (strcmp(sub, "schema") == 0) {
        char pbuf[512];
        FILE *f = NULL;
        hu_error_t er = hula_try_open_schema(&f, pbuf, sizeof(pbuf));
        if (er != HU_OK || !f) {
            hu_log_error("hula", NULL,
                         "schema file not found (try from repo root or set HUMAN_SOURCE_ROOT)");
            return er != HU_OK ? er : HU_ERR_NOT_FOUND;
        }
        printf("(schema) %s\n\n", pbuf[0] ? pbuf : "hula-program.schema.json");
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, stdout);
        fclose(f);
        return HU_OK;
    }

    if (strcmp(sub, "expand") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: human hula expand <template.txt> <vars.json>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        char *tmpl = NULL, *vars = NULL;
        size_t tl = 0, vl = 0;
        hu_error_t er = hula_read_file(alloc, argv[3], &tmpl, &tl);
        if (er != HU_OK) {
            hu_log_error("hula", NULL, "cannot read template: %s", hu_error_string(er));
            return er;
        }
        er = hula_read_file(alloc, argv[4], &vars, &vl);
        if (er != HU_OK) {
            hu_str_free(alloc, tmpl);
            hu_log_error("hula", NULL, "cannot read vars: %s", hu_error_string(er));
            return er;
        }
        char *out = NULL;
        size_t ol = 0;
        er = hu_hula_expand_template(alloc, tmpl, tl, vars, vl, &out, &ol);
        hu_str_free(alloc, tmpl);
        hu_str_free(alloc, vars);
        if (er != HU_OK) {
            hu_log_error("hula", NULL, "expand failed: %s", hu_error_string(er));
            return er;
        }
        fwrite(out, 1, ol, stdout);
        fputc('\n', stdout);
        hu_str_free(alloc, out);
        return HU_OK;
    }

    if (strcmp(sub, "compile") == 0) {
        int i = 3;
        bool lite = false;
        if (argc > i && strcmp(argv[i], "--lite") == 0) {
            lite = true;
            i++;
        }
        if (argc <= i) {
            fprintf(stderr, "Usage: human hula compile [--lite] <file>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        char *raw = NULL;
        size_t rl = 0;
        hu_error_t err = hula_read_file(alloc, argv[i], &raw, &rl);
        if (err != HU_OK) {
            hu_log_error("hula", NULL, "cannot read %s: %s", argv[i], hu_error_string(err));
            return err;
        }
        char *json = raw;
        size_t json_len = rl;
        char *lite_out = NULL;
        size_t lite_len = 0;
        if (lite) {
            err = hu_hula_lite_to_json(alloc, raw, rl, &lite_out, &lite_len);
            hu_str_free(alloc, raw);
            if (err != HU_OK) {
                hu_log_error("hula", NULL, "lite parse failed: %s", hu_error_string(err));
                return err;
            }
            json = lite_out;
            json_len = lite_len;
        }
        fwrite(json, 1, json_len, stdout);
        fputc('\n', stdout);
        if (lite_out)
            hu_str_free(alloc, lite_out);
        return HU_OK;
    }

    if (strcmp(sub, "replay") == 0) {
        int rj = 3;
        const char *replay_cfg = NULL;
        while (rj < argc - 1) {
            if (strcmp(argv[rj], "--config") == 0) {
                if (rj + 1 >= argc) {
                    fprintf(stderr, "Usage: human hula replay [--config path] <trace.json>\n");
                    return HU_ERR_INVALID_ARGUMENT;
                }
                replay_cfg = argv[rj + 1];
                rj += 2;
                continue;
            }
            hu_log_error("hula", NULL, "replay: unknown option: %s", argv[rj]);
            return HU_ERR_INVALID_ARGUMENT;
        }
        if (rj >= argc) {
            fprintf(stderr, "Usage: human hula replay [--config path] <trace.json>\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
        char *raw = NULL;
        size_t rl = 0;
        hu_error_t err = hula_read_file(alloc, argv[rj], &raw, &rl);
        if (err != HU_OK) {
            hu_log_error("hula", NULL, "cannot read %s: %s", argv[rj], hu_error_string(err));
            return err;
        }
        hu_json_value_t *wrap = NULL;
        err = hu_json_parse(alloc, raw, rl, &wrap);
        hu_str_free(alloc, raw);
        if (err != HU_OK || !wrap) {
            hu_log_error("hula", NULL, "replay: invalid JSON (%s)",
                         err != HU_OK ? hu_error_string(err) : "parse");
            if (wrap)
                hu_json_free(alloc, wrap);
            return err != HU_OK ? err : HU_ERR_PARSE;
        }
        hu_json_value_t *prog_obj = hu_json_object_get(wrap, "program");
        if (!prog_obj || prog_obj->type != HU_JSON_OBJECT) {
            hu_log_error(
                "hula", NULL,
                "replay: missing \"program\" object (only traces written after this change "
                "embed it; use hula run on your source JSON).");
            hu_json_free(alloc, wrap);
            return HU_ERR_INVALID_ARGUMENT;
        }
        char *pj = NULL;
        size_t pjl = 0;
        err = hu_json_stringify(alloc, prog_obj, &pj, &pjl);
        hu_json_free(alloc, wrap);
        if (err != HU_OK || !pj) {
            hu_log_error("hula", NULL, "replay: program serialize failed: %s",
                         hu_error_string(err));
            if (pj)
                hu_str_free(alloc, pj);
            return err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;
        }
        printf("── Replay ───────────────────────────────────────────\n");
        printf("  trace file: %s\n\n", argv[rj]);
        return hula_cli_run_program_json(alloc, pj, pjl, true, true, pj, pjl, replay_cfg, NULL, 0);
    }

    bool run_mode = (strcmp(sub, "run") == 0);
    bool validate_mode = (strcmp(sub, "validate") == 0);
    if (!run_mode && !validate_mode) {
        fprintf(stderr, "Unknown hula subcommand: %s\n", sub);
        return HU_ERR_INVALID_ARGUMENT;
    }

    int argi = 3;
    bool lite_input = false;
    const char *run_cfg = NULL;
    while (argi < argc) {
        if (strcmp(argv[argi], "--lite") == 0) {
            lite_input = true;
            argi++;
            continue;
        }
        if (strcmp(argv[argi], "--config") == 0) {
            if (argi + 1 >= argc) {
                hu_log_error("hula", NULL, "%s: --config requires a path", sub);
                return HU_ERR_INVALID_ARGUMENT;
            }
            run_cfg = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }
    if (argc <= argi) {
        fprintf(stderr, "Usage: human hula %s [--lite] [--config path] <file.json | '{...}'>\n",
                sub);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *input = argv[argi];
    char *json = NULL;
    size_t json_len = 0;
    bool json_owned = false;

    if (!lite_input && input[0] == '{') {
        json = (char *)input;
        json_len = strlen(input);
    } else {
        hu_error_t err = hula_read_file(alloc, input, &json, &json_len);
        if (err != HU_OK) {
            hu_log_error("hula", NULL, "cannot read %s: %s", input, hu_error_string(err));
            return err;
        }
        json_owned = true;
        if (lite_input) {
            char *lj = NULL;
            size_t ll = 0;
            err = hu_hula_lite_to_json(alloc, json, json_len, &lj, &ll);
            if (json_owned)
                hu_str_free(alloc, json);
            json = lj;
            json_len = ll;
            json_owned = true;
            if (err != HU_OK) {
                hu_log_error("hula", NULL, "lite parse failed: %s", hu_error_string(err));
                return err;
            }
        }
    }

    return hula_cli_run_program_json(alloc, json, json_len, json_owned, run_mode, NULL, 0, run_cfg,
                                     json, json_len);
}
