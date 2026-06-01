#include "human/agent/cli.h"
#include "human/agent.h"
#include "human/agent/awareness.h"
#include "human/agent/outcomes.h"
#include "human/agent/profile.h"
#include "human/agent/spawn.h"
#include "human/agent/tui.h"
#include "human/channels/cli.h"
#include "human/core/log.h"
#ifdef HU_HAS_VOICE_CHANNEL
#include "human/channels/voice_channel.h"
#endif
#include "human/cognition/metacognition.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/string.h"
#ifdef HU_ENABLE_APPLE_INTELLIGENCE
#include "human/providers/apple.h"
#endif
#ifdef HU_HAS_CRON
#include "human/cron.h"
#endif
#include "human/bus.h"
#include "human/design_tokens.h"
/* Legacy `hu_memory_t` comes via `human/agent.h` → `human/memory.h`. Do not
 * include `human/memory.h` in the same TU as `human/memory/memory.h` (G2 gate). */
#include "human/memory/engines.h"
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
#include "human/feeds/findings.h"
#include "human/feeds/processor.h"
#include "human/feeds/research.h"
#include "human/feeds/trends.h"
#include "human/intelligence/cycle.h"
#include "human/intelligence/online_learning.h"
#include "human/intelligence/self_improve.h"
#endif
#include "human/memory/factory.h"
#include "human/memory/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/observability/log_observer.h"
#ifdef HU_HAS_OTEL
#include "human/observability/otel.h"
#endif
#include "human/agent/model_router.h"
#include "human/agent/session_persist.h"
#include "human/plugin.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/runtime.h"
#include "human/security.h"
#include "human/security/audit.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include "human/tool.h"
#include "human/tools/factory.h"
#include "human/version.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if !defined(HU_IS_TEST) && defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif

#if defined(HU_GATEWAY_POSIX) && !defined(HU_IS_TEST)
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#define HU_CLI_ASYNC 1
#else
#define HU_CLI_ASYNC 0
#endif

#define HU_CODENAME     "Human"
#define HU_CLI_MAX_PATH 1024

#ifndef HU_IS_TEST
/* Gemini-tier router IDs are only valid for Google providers; OpenAI et al. need cfg model names.
 */
static bool cli_provider_uses_gemini_slot_models(const char *prov, size_t prov_len) {
    if (!prov || prov_len == 0)
        return false;
    char buf[12];
    if (prov_len >= sizeof(buf))
        return false;
    for (size_t i = 0; i < prov_len; i++)
        buf[i] = (char)tolower((unsigned char)prov[i]);
    buf[prov_len] = '\0';
    return strcmp(buf, "gemini") == 0 || strcmp(buf, "google") == 0 || strcmp(buf, "vertex") == 0;
}
#endif

/* ── ANSI escape helpers (from design_tokens.h) ─────────────────────── */
#define HU_ANSI_HIDE_CURSOR "\033[?25l"
#define HU_ANSI_SHOW_CURSOR "\033[?25h"
#define HU_ANSI_CLEAR_LINE  "\033[2K\r"

#if HU_CLI_ASYNC
static const char *spinner_frames[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
    "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"};
#define SPINNER_FRAME_COUNT 10
#endif

/* ── MLX local server auto-start (macOS only) ────────────────────────── */
#if !defined(HU_IS_TEST) && defined(__APPLE__)
static bool mlx_port_is_open(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    bool ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

static bool mlx_auto_serve(const char *prov_name) {
    if (!prov_name)
        return true;
    if (strcmp(prov_name, "mlx_local") != 0 && strcmp(prov_name, "mlx-local") != 0)
        return true;
    if (mlx_port_is_open(8741))
        return true;

    hu_log_info("human", NULL, "MLX server not running — auto-starting...");
    fprintf(stderr,
            "\033[90mStarting MLX model server (this may take a moment on first run)...\033[0m\n");

    const char *home = getenv("HOME");
    if (!home)
        home = "/tmp";

    /* G16 (2026-05-26): search candidates in deploy-priority order. ~/Documents/h-uman
     * was the historic dev-clone path but moved to ~/Projects/h-uman around Apr 2026;
     * keep Documents in the list for backwards compatibility (one-and-done; nothing
     * to migrate if a user still has an old clone there) but place Projects first
     * so a fresh install resolves to the canonical location. */
    char candidates[4][HU_CLI_MAX_PATH];
    size_t ncand = 0;
    snprintf(candidates[ncand++], HU_CLI_MAX_PATH, "%s/.human/bin/human-serve.sh", home);
    snprintf(candidates[ncand++], HU_CLI_MAX_PATH, "%s/Projects/h-uman/scripts/human-serve.sh",
             home);
    snprintf(candidates[ncand++], HU_CLI_MAX_PATH, "%s/Documents/h-uman/scripts/human-serve.sh",
             home);
    snprintf(candidates[ncand++], HU_CLI_MAX_PATH, "%s/h-uman/scripts/human-serve.sh", home);

    const char *found = NULL;
    for (size_t i = 0; i < ncand; i++) {
        if (access(candidates[i], X_OK) == 0) {
            found = candidates[i];
            break;
        }
    }

    if (!found) {
        hu_log_error("human", NULL,
                     "Cannot find human-serve.sh. Start the server manually:\n"
                     "  scripts/human-serve.sh start");
        return false;
    }

    extern char **environ;
    char *argv[] = {(char *)found, "ensure", NULL};
    pid_t pid = 0;
    int spawn_err = posix_spawn(&pid, found, NULL, NULL, argv, environ);
    if (spawn_err != 0) {
        hu_log_error("human", NULL,
                     "MLX server failed to start. Run manually:\n  scripts/human-serve.sh start");
        return false;
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    int rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;

    if (rc != 0) {
        hu_log_error("human", NULL,
                     "MLX server failed to start. Run manually:\n  scripts/human-serve.sh start");
        return false;
    }

    /* Verify the port is now open */
    for (int i = 0; i < 5; i++) {
        if (mlx_port_is_open(8741))
            return true;
        usleep(500000);
    }
    hu_log_error("human", NULL, "MLX server started but port 8741 not responding");
    return false;
}
#endif /* !HU_IS_TEST && __APPLE__ */

/* ── Global cancel flag (set by SIGINT handler) ──────────────────────── */
static volatile sig_atomic_t g_cancel = 0;
static volatile sig_atomic_t g_reload_requested = 0;
static hu_agent_t *g_active_agent = NULL;

static void sigint_handler(int sig) {
    (void)sig;
    /* Async-signal-safe: only sig_atomic_t; agent cancel is propagated in the main loop. */
    g_cancel = 1;
}

#ifndef _WIN32
static void sighup_handler(int sig) {
    (void)sig;
    g_reload_requested = 1;
}
#endif

/* ── Memory from config — delegates to shared factory ────────────────── */

/* ── Arg parsing ─────────────────────────────────────────────────────── */
hu_error_t hu_agent_cli_parse_args(const char *const *argv, size_t argc,
                                   hu_parsed_agent_args_t *out) {
    if (!argv || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--config") == 0) {
            if (i + 1 < argc) {
                out->config_path = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "-m") == 0 || strcmp(a, "--message") == 0) {
            if (i + 1 < argc) {
                out->message = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--session") == 0) {
            if (i + 1 < argc) {
                out->session_id = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "--contact") == 0) {
            if (i + 1 < argc) {
                out->contact_id = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "--provider") == 0) {
            if (i + 1 < argc) {
                out->provider_override = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "--model") == 0) {
            if (i + 1 < argc) {
                out->model_override = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "--temperature") == 0) {
            if (i + 1 < argc) {
                int parsed = sscanf(argv[i + 1], "%lf", &out->temperature_override);
                if (parsed == 1 && out->temperature_override >= 0.0 &&
                    out->temperature_override <= 2.0) {
                    out->has_temperature = 1;
                }
                i++;
            }
        } else if (strcmp(a, "--tui") == 0) {
            out->use_tui = 1;
        } else if (strcmp(a, "--demo") == 0) {
            out->demo_mode = 1;
        } else if (strcmp(a, "--prompt") == 0) {
            if (i + 1 < argc) {
                out->prompt = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "--channel") == 0) {
            if (i + 1 < argc) {
                out->channel = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "--once") == 0) {
            out->once = 1;
        }
    }
    return HU_OK;
}

/* ── Streaming token callback ────────────────────────────────────────── */
static volatile sig_atomic_t cli_stream_started = 0;
static int g_cli_use_ansi = 0;

static void cli_stream_token(const char *delta, size_t len, void *ctx) {
    (void)ctx;
    if (delta && len > 0) {
        if (!cli_stream_started) {
            cli_stream_started = 1;
            if (g_cli_use_ansi)
                printf(HU_ANSI_SHOW_CURSOR HU_ANSI_CLEAR_LINE);
        }
        fwrite(delta, 1, len, stdout);
        fflush(stdout);
    }
}

/* ── Background agent turn (async mode) ──────────────────────────────── */
#if HU_CLI_ASYNC
typedef struct agent_turn_ctx {
    hu_agent_t *agent;
    const char *msg;
    size_t msg_len;
    char *response;
    size_t response_len;
    hu_error_t err;
    volatile int done;
} agent_turn_ctx_t;

static void *agent_turn_thread(void *arg) {
    agent_turn_ctx_t *ctx = (agent_turn_ctx_t *)arg;
    ctx->agent->active_channel = "cli";
    ctx->agent->active_channel_len = 3;
    ctx->err = hu_agent_turn_stream(ctx->agent, ctx->msg, ctx->msg_len, cli_stream_token, NULL,
                                    &ctx->response, &ctx->response_len);
    ctx->done = 1;
    return NULL;
}

static int get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static void run_spinner_loop(agent_turn_ctx_t *tctx, int use_ansi) {
    int frame = 0;
    if (use_ansi) {
        printf(HU_ANSI_HIDE_CURSOR);
        fflush(stdout);
    }

    while (!tctx->done && !g_cancel) {
        if (use_ansi && !cli_stream_started) {
            int width = get_terminal_width();
            const char *label = " Thinking...";
            printf(HU_ANSI_CLEAR_LINE HU_COLOR_ACCENT "%s" HU_COLOR_RESET HU_COLOR_DIM
                                                      "%s" HU_COLOR_RESET,
                   spinner_frames[frame % SPINNER_FRAME_COUNT], label);
            (void)width;
            fflush(stdout);
            frame++;
        }

        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        poll(&pfd, 1, 80);
    }

    if (use_ansi) {
        if (!cli_stream_started) {
            printf(HU_ANSI_CLEAR_LINE HU_ANSI_SHOW_CURSOR);
            fflush(stdout);
        } else {
            printf(HU_ANSI_SHOW_CURSOR);
            fflush(stdout);
        }
    }
}
#endif /* HU_CLI_ASYNC */

/* ── Print welcome banner ────────────────────────────────────────────── */
static void print_banner(const char *prov_name, const char *model, size_t tools_count) {
    printf(HU_COLOR_BOLD HU_COLOR_ACCENT "%s" HU_COLOR_RESET " v%s" HU_COLOR_DIM
                                         " — not quite human." HU_COLOR_RESET "\n",
           HU_CODENAME, hu_version_string());
    printf(HU_COLOR_DIM "Provider: %s | Model: %s | Tools: %zu" HU_COLOR_RESET "\n", prov_name,
           (model[0] ? model : "(default)"), tools_count);
    printf("Type your message, or " HU_COLOR_DIM "'exit'" HU_COLOR_RESET " to leave. " HU_COLOR_DIM
           "Ctrl+C cancels a running turn." HU_COLOR_RESET "\n\n");
}

/* ── Main CLI loop ───────────────────────────────────────────────────── */
hu_error_t hu_agent_cli_run(hu_allocator_t *alloc, const char *const *argv, size_t argc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    hu_parsed_agent_args_t parsed_args;
    hu_agent_cli_parse_args(argv, argc, &parsed_args);

    const char *config_path =
        parsed_args.config_path ? parsed_args.config_path : getenv("HUMAN_CONFIG_PATH");
    hu_config_t cfg;
#ifdef HU_HAS_VOICE_CHANNEL
    hu_channel_t cli_voice_ch = {0};
#endif
    hu_error_t err = (config_path && config_path[0]) ? hu_config_load_from(alloc, config_path, &cfg)
                                                     : hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "config error: %s", hu_error_string(err));
        return err;
    }

    if (parsed_args.demo_mode) {
        hu_log_info("human", NULL, "Demo mode — using local Ollama (no API key required)");
        hu_log_info("human", NULL, "Make sure Ollama is running: ollama serve");
        cfg.default_provider = "ollama";
        cfg.default_model = "llama3.2";
        cfg.memory.backend = "none";
        cfg.memory_backend = "none";
    }

    /* Auto-load default persona when none configured (activates personality layer) */
    if (!cfg.agent.persona || !cfg.agent.persona[0]) {
        hu_allocator_t *ca = &cfg.allocator;
        if (cfg.agent.persona)
            ca->free(ca->ctx, cfg.agent.persona, strlen(cfg.agent.persona) + 1);
        cfg.agent.persona = hu_strdup(ca, "default");
    }

    const char *prov_name = cfg.default_provider ? cfg.default_provider : "openai";

#if !defined(HU_IS_TEST) && defined(__APPLE__)
    if (!mlx_auto_serve(prov_name)) {
        hu_config_deinit(&cfg);
        return HU_ERR_PROVIDER_UNAVAILABLE;
    }
#endif

    size_t prov_name_len = strlen(prov_name);
    /* Use hu_provider_create_default so interactive `human agent` sessions
     * also get the auto-fallback wrapping that the daemon path enjoys.
     * If cfg->reliability.fallback_providers is set, the provider is
     * transparently wrapped in a self-healing reliable composite. */
    hu_provider_t provider;
    err = hu_provider_create_default(alloc, &cfg, &provider);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "provider '%s' init failed: %s", prov_name,
                     hu_error_string(err));
#ifdef HU_HAS_VOICE_CHANNEL
        hu_channel_voice_destroy(&cli_voice_ch);
#endif
        hu_config_deinit(&cfg);
        return err;
    }

    const char *model = cfg.default_model ? cfg.default_model : "";
    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    double temp = cfg.temperature > 0.0 ? cfg.temperature : 0.7;
    uint32_t max_iters = cfg.agent.max_tool_iterations > 0 ? cfg.agent.max_tool_iterations : 25;
    uint32_t max_hist = cfg.agent.max_history_messages > 0 ? cfg.agent.max_history_messages : 100;

    hu_runtime_t runtime;
    err = hu_runtime_from_config(&cfg, &runtime);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "runtime '%s' not supported: %s",
                     cfg.runtime.kind ? cfg.runtime.kind : "(null)", hu_error_string(err));
#ifdef HU_HAS_VOICE_CHANNEL
        hu_channel_voice_destroy(&cli_voice_ch);
#endif
        hu_config_deinit(&cfg);
        return err;
    }

    hu_security_policy_t policy = {0};
    policy.autonomy = (hu_autonomy_level_t)cfg.security.autonomy_level;
    policy.workspace_dir = ws;
    policy.workspace_only = true;
    policy.allow_shell = (policy.autonomy != HU_AUTONOMY_READ_ONLY);
    if (runtime.vtable && runtime.vtable->has_shell_access)
        policy.allow_shell = policy.allow_shell && runtime.vtable->has_shell_access(runtime.ctx);
    policy.block_high_risk_commands = (policy.autonomy == HU_AUTONOMY_SUPERVISED);
    policy.require_approval_for_medium_risk = (policy.autonomy == HU_AUTONOMY_SUPERVISED);
    policy.max_actions_per_hour = 100;
    policy.tracker = hu_rate_tracker_create(alloc, policy.max_actions_per_hour);

    hu_sandbox_alloc_t sb_alloc = {
        .ctx = alloc->ctx,
        .alloc = alloc->alloc,
        .free = alloc->free,
    };
    hu_sandbox_storage_t *sb_storage = NULL;
    hu_sandbox_t sandbox = {0};
    hu_net_proxy_t net_proxy = {0};

    if (cfg.security.sandbox_config.enabled ||
        cfg.security.sandbox_config.backend != HU_SANDBOX_NONE) {
        sb_storage = hu_sandbox_storage_create(&sb_alloc);
        if (sb_storage) {
            sandbox =
                hu_sandbox_create(cfg.security.sandbox_config.backend, ws, sb_storage, &sb_alloc);
            if (sandbox.vtable) {
                policy.sandbox = &sandbox;
#if defined(__linux__)
                if (strcmp(hu_sandbox_name(&sandbox), "firejail") == 0 &&
                    cfg.security.sandbox_config.firejail_args_len > 0) {
                    hu_firejail_sandbox_set_extra_args(
                        (hu_firejail_ctx_t *)sandbox.ctx,
                        (const char *const *)cfg.security.sandbox_config.firejail_args,
                        cfg.security.sandbox_config.firejail_args_len);
                }
#endif
            }
        }
    }

    if (cfg.security.sandbox_config.net_proxy.enabled) {
        net_proxy.enabled = true;
        net_proxy.deny_all = cfg.security.sandbox_config.net_proxy.deny_all;
        net_proxy.proxy_addr = cfg.security.sandbox_config.net_proxy.proxy_addr;
        net_proxy.allowed_domains_count = 0;
        for (size_t i = 0; i < cfg.security.sandbox_config.net_proxy.allowed_domains_len &&
                           i < HU_NET_PROXY_MAX_DOMAINS;
             i++) {
            net_proxy.allowed_domains[net_proxy.allowed_domains_count++] =
                cfg.security.sandbox_config.net_proxy.allowed_domains[i];
        }
        policy.net_proxy = &net_proxy;
    }

    hu_observer_t observer = {0};
    FILE *log_fp = NULL;
    const char *log_env = getenv("HUMAN_LOG");
    if (log_env && log_env[0]) {
        log_fp = fopen(log_env, "a");
        if (!log_fp) {
            hu_log_error("warn", NULL, "could not open log file '%s': %s", log_env,
                         strerror(errno));
        } else {
            observer = hu_log_observer_create(alloc, log_fp);
        }
    }

    hu_bus_t cli_bus;
    hu_bus_init(&cli_bus);
    hu_awareness_t cli_awareness = {0};
    (void)hu_awareness_init(&cli_awareness, &cli_bus);

    hu_memory_t memory = hu_memory_create_from_config(alloc, &cfg, ws);
    hu_session_store_t session_store = {0};
    if (memory.vtable &&
        strcmp(cfg.memory.backend ? cfg.memory.backend : "markdown", "sqlite") == 0)
        session_store = hu_sqlite_memory_get_session_store(&memory);

    hu_embedder_t embedder = hu_embedder_local_create(alloc);
    hu_vector_store_t vector_store = hu_vector_store_mem_create(alloc);
    hu_retrieval_engine_t retrieval_engine =
        hu_retrieval_create_with_vector(alloc, &memory, &embedder, &vector_store);
#ifdef HU_ENABLE_SQLITE
    struct hu_graph *cli_graph = NULL;
#endif

#ifdef HU_HAS_CRON
    hu_cron_scheduler_t *cron = hu_cron_create(alloc, 64, true);
#else
    hu_cron_scheduler_t *cron = NULL;
#endif

    hu_agent_pool_t *cli_agent_pool = hu_agent_pool_create(alloc, cfg.agent.pool_max_concurrent);
    {
        hu_fleet_limits_t fl = {0};
        fl.max_spawn_depth = cfg.agent.fleet_max_spawn_depth;
        fl.max_total_spawns = cfg.agent.fleet_max_total_spawns;
        fl.budget_limit_usd = cfg.agent.fleet_budget_usd;
        hu_agent_pool_set_fleet_limits(cli_agent_pool, &fl);
    }
    hu_mailbox_t *cli_mailbox = hu_mailbox_create(alloc, 64);

    hu_tool_t *tools = NULL;
    size_t tools_count = 0;
    err = hu_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,
                                  memory.vtable ? &memory : NULL, cron, cli_agent_pool, cli_mailbox,
                                  NULL, NULL, &tools, &tools_count);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "tools init failed: %s", hu_error_string(err));
        if (cron)
            hu_cron_destroy(cron, alloc);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
#ifdef HU_ENABLE_SQLITE
        if (cli_graph) {
            hu_memory_v1_graph_close(cli_graph, alloc);
            cli_graph = NULL;
        }
#endif
        if (vector_store.vtable && vector_store.vtable->deinit)
            vector_store.vtable->deinit(vector_store.ctx, alloc);
        if (embedder.vtable && embedder.vtable->deinit)
            embedder.vtable->deinit(embedder.ctx, alloc);
        if (memory.vtable && memory.vtable->deinit)
            memory.vtable->deinit(memory.ctx);
        if (policy.tracker)
            hu_rate_tracker_destroy(policy.tracker);
        if (sb_storage)
            hu_sandbox_storage_destroy(sb_storage, &sb_alloc);
        hu_awareness_deinit(&cli_awareness);
        hu_bus_deinit(&cli_bus);
#ifdef HU_HAS_VOICE_CHANNEL
        hu_channel_voice_destroy(&cli_voice_ch);
#endif
        hu_config_deinit(&cfg);
        return err;
    }

    hu_agent_context_config_t ctx_cfg = {
        .token_limit = cfg.agent.token_limit,
        .pressure_warn = cfg.agent.context_pressure_warn,
        .pressure_compact = cfg.agent.context_pressure_compact,
        .compact_target = cfg.agent.context_compact_target,
        .compact_context = cfg.agent.compact_context,
        .llm_compiler_enabled = cfg.agent.llm_compiler_enabled,
        .hula_enabled = cfg.agent.hula_enabled,
        .mcts_planner_enabled = cfg.agent.mcts_planner_enabled,
        .tree_of_thought = cfg.agent.tree_of_thought,
        .constitutional_ai = cfg.agent.constitutional_ai,
        .constitutional_style_rules_enabled = cfg.agent.constitutional_style_rules_enabled,
        .speculative_cache = cfg.agent.speculative_cache,
        .tool_routing_enabled = cfg.agent.tool_routing_enabled,
        .multi_agent = cfg.agent.multi_agent,
        .compaction_use_structured = cfg.agent.compaction_use_structured,
    };
    /* 2026-05-27 — heap-allocate the agent struct so its address is NOT on
     * T0's stack. The cross-thread pattern (T0's pthread_create passes
     * agent_p into tctx; T1's agent_turn_thread reads agent_p->X through
     * that pointer; hu_agent_free_turn_context inside hu_agent_turn writes
     * back to agent_p->humanness_ctx_buf) trips the SAME ASan stack-buffer-
     * overflow false positive that the tctx fix (Chip G, commit 3dd1d13c)
     * addressed for loop-scope locals. Heap storage has its own shadow-
     * region lifecycle keyed to malloc/free, dodging pthread-stack mmap
     * reuse on Darwin arm64.
     *
     * See .claude/rules/asan-pthread-stack-aliasing-darwin.md — this is the
     * "function-scope" follow-up that completes the Chip G investigation.
     * Free-on-exit is done at every return path below; if you add a new
     * return after this allocation, free agent_p first or you leak. */
    hu_agent_t *agent_p = (hu_agent_t *)alloc->alloc(alloc->ctx, sizeof(*agent_p));
    if (!agent_p) {
        hu_log_error("human", NULL, "agent storage alloc failed");
        hu_config_deinit(&cfg);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(agent_p, 0, sizeof(*agent_p));
    err = hu_agent_from_config(
        agent_p, alloc, provider, tools, tools_count, memory.vtable ? &memory : NULL,
        session_store.vtable ? &session_store : NULL, &observer, NULL, model, strlen(model),
        prov_name, prov_name_len, temp, ws, strlen(ws), max_iters, max_hist, cfg.memory.auto_save,
        2, NULL, 0, cfg.agent.persona, cfg.agent.persona ? strlen(cfg.agent.persona) : 0, &ctx_cfg);
    if (err != HU_OK) {
        hu_log_error("human", NULL, "agent init failed: %s", hu_error_string(err));
        if (observer.vtable && observer.vtable->deinit)
            observer.vtable->deinit(observer.ctx);
        if (log_fp)
            fclose(log_fp);
        hu_tools_destroy_default(alloc, tools, tools_count);
        if (cron)
            hu_cron_destroy(cron, alloc);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
#ifdef HU_ENABLE_SQLITE
        if (cli_graph) {
            hu_memory_v1_graph_close(cli_graph, alloc);
            cli_graph = NULL;
        }
#endif
        if (vector_store.vtable && vector_store.vtable->deinit)
            vector_store.vtable->deinit(vector_store.ctx, alloc);
        if (embedder.vtable && embedder.vtable->deinit)
            embedder.vtable->deinit(embedder.ctx, alloc);
        if (policy.tracker)
            hu_rate_tracker_destroy(policy.tracker);
        if (sb_storage)
            hu_sandbox_storage_destroy(sb_storage, &sb_alloc);
        hu_awareness_deinit(&cli_awareness);
        hu_bus_deinit(&cli_bus);
#ifdef HU_HAS_VOICE_CHANNEL
        hu_channel_voice_destroy(&cli_voice_ch);
#endif
        hu_config_deinit(&cfg);
        alloc->free(alloc->ctx, agent_p, sizeof(*agent_p));
        return err;
    }
    /* 2026-05 Chip F — borrow the loaded cfg so per-turn paths (agent_turn.c,
     * agent_stream.c, media_gif.c, media_video.c) can read runtime config
     * fields like prompt_budget.enabled and media_gen.vertex_project. Without
     * this assignment, the per-turn ternary `(*agent_p)->config ? ... : false`
     * evaluates to false even when the operator has explicitly enabled the
     * feature in config.json — discovered during Chip D as the deeper bug
     * behind the misleading prompt_budget boot warning. `cfg` outlives
     * `(*agent_p)` (cfg lives in this function's stack frame which encloses the
     * (*agent_p)'s lifetime; agent_p->config is a borrowed pointer, not owned). */
    agent_p->config = &cfg;
    hu_metacognition_apply_config(&agent_p->infra.metacognition, &cfg.agent.metacognition);
    hu_voice_config_t voice_cfg = {0};
    (void)hu_voice_config_from_settings(&cfg, &voice_cfg);
    if (voice_cfg.tts_provider || voice_cfg.local_tts_endpoint || voice_cfg.api_key ||
        voice_cfg.cartesia_api_key || voice_cfg.openai_api_key ||
        (cfg.voice.mode && cfg.voice.mode[0])) {
        hu_agent_set_voice_config(agent_p, &voice_cfg);
    }
#ifdef HU_HAS_VOICE_CHANNEL
    if (cfg.voice.mode && cfg.voice.mode[0]) {
        hu_channel_voice_config_t vcfg = {0};
        bool want_voice = false;
        if (strcmp(cfg.voice.mode, "realtime") == 0) {
            vcfg.mode = HU_VOICE_MODE_REALTIME;
            vcfg.api_key = hu_config_get_provider_key(&cfg, "openai");
            vcfg.model = cfg.voice.realtime_model;
            vcfg.voice = cfg.voice.realtime_voice;
            want_voice = true;
        } else if (strcmp(cfg.voice.mode, "webrtc") == 0) {
            vcfg.mode = HU_VOICE_MODE_WEBRTC;
            want_voice = true;
        } else if (strcmp(cfg.voice.mode, "sonata") == 0) {
            vcfg.mode = HU_VOICE_MODE_SONATA;
            want_voice = true;
        }
        if (want_voice)
            (void)hu_channel_voice_create(alloc, &vcfg, &cli_voice_ch);
    }
#endif
    agent_p->agent_pool = cli_agent_pool;
    hu_agent_set_mailbox(agent_p, cli_mailbox);
    agent_p->policy_engine = NULL;

    if (cfg.security.audit.enabled) {
        hu_audit_config_t acfg = HU_AUDIT_CONFIG_DEFAULT;
        acfg.enabled = true;
        acfg.log_path = cfg.security.audit.log_path ? cfg.security.audit.log_path : "audit.log";
        acfg.max_size_mb = cfg.security.audit.max_size_mb > 0 ? cfg.security.audit.max_size_mb : 10;
        agent_p->audit_logger = hu_audit_logger_create(alloc, &acfg, ws);
    }

    if (cfg.policy.enabled) {
        agent_p->policy_engine = hu_policy_engine_create(alloc);
    }

    if (cfg.agent.default_profile) {
        const hu_agent_profile_t *prof =
            hu_agent_profile_by_name(cfg.agent.default_profile, strlen(cfg.agent.default_profile));
        if (prof) {
            if (prof->preferred_model && prof->preferred_model[0] && !parsed_args.model_override) {
                char *old = agent_p->model_name;
                size_t old_len = agent_p->model_name_len;
                agent_p->model_name =
                    hu_strndup(alloc, prof->preferred_model, strlen(prof->preferred_model));
                agent_p->model_name_len = strlen(prof->preferred_model);
                if (old)
                    alloc->free(alloc->ctx, old, old_len + 1);
            }
            if (prof->temperature > 0)
                agent_p->temperature = prof->temperature;
            if (prof->max_iterations > 0)
                agent_p->max_tool_iterations = prof->max_iterations;
            if (prof->max_history > 0)
                agent_p->max_history_messages = prof->max_history;
        }
    }

#ifdef HU_HAS_OTEL
    hu_observer_t otel_observer = {0};
    if (cfg.diagnostics.otel_endpoint && cfg.diagnostics.otel_endpoint[0]) {
        hu_otel_config_t otel_cfg = {
            .endpoint = cfg.diagnostics.otel_endpoint,
            .endpoint_len = strlen(cfg.diagnostics.otel_endpoint),
            .service_name =
                cfg.diagnostics.otel_service_name ? cfg.diagnostics.otel_service_name : "human",
            .service_name_len =
                cfg.diagnostics.otel_service_name ? strlen(cfg.diagnostics.otel_service_name) : 7,
            .enable_traces = true,
            .enable_metrics = true,
            .enable_logs = true,
        };
        if (hu_otel_observer_create(alloc, &otel_cfg, &otel_observer) == HU_OK &&
            otel_observer.vtable) {
            agent_p->observer = &otel_observer;
        }
    }
#endif

    hu_agent_set_retrieval_engine(agent_p, &retrieval_engine);
#ifdef HU_ENABLE_SQLITE
    {
        const char *home = getenv("HOME");
        if (home) {
            char graph_path[1024];
            int np = snprintf(graph_path, sizeof(graph_path), "%s/.human/graph.db", home);
            if (np > 0 && (size_t)np < sizeof(graph_path))
                (void)hu_memory_v1_graph_open(alloc, graph_path, (size_t)np, &cli_graph);
        }
    }
    if (cli_graph) {
        hu_retrieval_set_graph(&retrieval_engine, cli_graph);
        hu_error_t ge = hu_agent_bind_sqlite_graph(agent_p, cli_graph, alloc);
        if (ge != HU_OK)
            hu_log_warn("human", NULL, "CLI graph bind: %s", hu_error_string(ge));
    }
#endif
    if (cli_awareness.bus)
        hu_agent_set_awareness(agent_p, (struct hu_awareness *)&cli_awareness);

    hu_outcome_tracker_t cli_outcomes;
    hu_outcome_tracker_init(&cli_outcomes, true);
    hu_agent_set_outcomes(agent_p, &cli_outcomes);
    agent_p->scheduler = (struct hu_cron_scheduler *)cron;

    if (parsed_args.message && parsed_args.message[0])
        agent_p->lean_prompt = true;

    /* --contact: bind the contact id as the memory session so per-contact memory
     * recall AND GraphRAG community-summary grounding fire on a one-shot CLI turn
     * (agent_turn gates grounding on memory_session_id && len>0). The daemon binds
     * this from cp->contact_id; the CLI had no equivalent seam. memory_session_id
     * is a non-owning const char* (see daemon.c) and argv outlives the turn, so
     * pointing it at the flag string is safe. */
    if (parsed_args.contact_id && parsed_args.contact_id[0]) {
        agent_p->memory_session_id = parsed_args.contact_id;
        agent_p->memory_session_id_len = strlen(parsed_args.contact_id);
        if (agent_p->memory && agent_p->memory->vtable)
            agent_p->memory->current_session_id = parsed_args.contact_id;
    }

    /* Session persistence: load prior conversation if --session was given,
     * or generate a session ID so auto_save works for new sessions. */
    {
        char sessions_dir[512];
        const char *home = getenv("HOME");
        if (home)
            snprintf(sessions_dir, sizeof(sessions_dir), "%s/.human/sessions", home);
        else
            snprintf(sessions_dir, sizeof(sessions_dir), ".human/sessions");
        if (parsed_args.session_id && parsed_args.session_id[0]) {
            size_t sid_len = strlen(parsed_args.session_id);
            if (sid_len < sizeof(agent_p->session_id)) {
                memcpy(agent_p->session_id, parsed_args.session_id, sid_len);
                agent_p->session_id[sid_len] = '\0';
                hu_error_t lerr =
                    hu_session_persist_load(alloc, agent_p, sessions_dir, parsed_args.session_id);
                if (lerr == HU_OK)
                    hu_log_info("human", NULL, "resumed session: %s", parsed_args.session_id);
                else
                    hu_log_info("human", NULL, "new session: %s", parsed_args.session_id);
            }
        } else if (agent_p->auto_save) {
            /* Generate a default session ID from timestamp so auto_save works */
            time_t now = time(NULL);
            int sn =
                snprintf(agent_p->session_id, sizeof(agent_p->session_id), "cli-%ld", (long)now);
            if (sn <= 0 || (size_t)sn >= sizeof(agent_p->session_id))
                agent_p->session_id[0] = '\0';
        }
    }

    /* TUI mode: launch split-pane terminal UI if --tui was passed */
    if (parsed_args.use_tui) {
        hu_tui_state_t tui_state;
        err = hu_tui_init(&tui_state, alloc, agent_p, prov_name, model, tools_count);
        if (err == HU_OK) {
            err = hu_tui_run(&tui_state);
            hu_tui_deinit(&tui_state);
        } else {
            hu_log_error("human", &observer, "TUI not available: %s", hu_error_string(err));
        }
        hu_agent_deinit(agent_p);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
#ifdef HU_ENABLE_SQLITE
        if (cli_graph) {
            hu_memory_v1_graph_close(cli_graph, alloc);
            cli_graph = NULL;
        }
#endif
        if (vector_store.vtable && vector_store.vtable->deinit)
            vector_store.vtable->deinit(vector_store.ctx, alloc);
        if (embedder.vtable && embedder.vtable->deinit)
            embedder.vtable->deinit(embedder.ctx, alloc);
        if (memory.vtable && memory.vtable->deinit)
            memory.vtable->deinit(memory.ctx);
        if (observer.vtable && observer.vtable->deinit)
            observer.vtable->deinit(observer.ctx);
        if (log_fp)
            fclose(log_fp);
        hu_tools_destroy_default(alloc, tools, tools_count);
#ifdef HU_HAS_OTEL
        if (otel_observer.vtable && otel_observer.vtable->deinit)
            otel_observer.vtable->deinit(otel_observer.ctx);
#endif
        if (agent_p->policy_engine)
            hu_policy_engine_destroy(agent_p->policy_engine);
        if (cli_mailbox)
            hu_mailbox_destroy(cli_mailbox);
        if (cli_agent_pool)
            hu_agent_pool_destroy(cli_agent_pool);
        if (policy.tracker)
            hu_rate_tracker_destroy(policy.tracker);
        if (sb_storage)
            hu_sandbox_storage_destroy(sb_storage, &sb_alloc);
        hu_awareness_deinit(&cli_awareness);
        hu_bus_deinit(&cli_bus);
#ifdef HU_HAS_VOICE_CHANNEL
        hu_channel_voice_destroy(&cli_voice_ch);
#endif
        hu_config_deinit(&cfg);
        alloc->free(alloc->ctx, agent_p, sizeof(*agent_p));
        return err;
    }

    if (parsed_args.once && parsed_args.prompt && parsed_args.prompt[0]) {
        const char *msg = parsed_args.message && parsed_args.message[0]
                              ? parsed_args.message
                              : "Analyze the provided context and report findings.";
        const char *chan =
            parsed_args.channel && parsed_args.channel[0] ? parsed_args.channel : "cli";
        agent_p->active_channel = chan;
        agent_p->active_channel_len = strlen(chan);

        const char *effective_prompt = parsed_args.prompt;
        char *enriched_prompt = NULL;
        size_t enriched_len = 0;
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
        {
            sqlite3 *feed_db = hu_sqlite_memory_get_db(&memory);
            if (feed_db) {
                char *digest = NULL;
                size_t digest_len = 0;
                int64_t since = (int64_t)time(NULL) - 86400;
                if (hu_feed_build_daily_digest(alloc, feed_db, since, 2000, &digest, &digest_len) ==
                        HU_OK &&
                    digest) {
                    char *trend_section = NULL;
                    size_t trend_len = 0;
                    if (cfg.feeds.interests) {
                        hu_feed_trend_t *trends = NULL;
                        size_t trend_count = 0;
                        if (hu_feed_detect_trends(alloc, feed_db, cfg.feeds.interests,
                                                  strlen(cfg.feeds.interests), &trends,
                                                  &trend_count) == HU_OK) {
                            hu_feed_trends_build_section(alloc, trends, trend_count, &trend_section,
                                                         &trend_len);
                            hu_feed_trends_free(alloc, trends, trend_count);
                        }
                    }

                    char *patches = NULL;
                    size_t patches_len = 0;
                    hu_self_improve_t si = {0};
                    if (hu_self_improve_create(alloc, feed_db, &si) == HU_OK) {
                        (void)hu_self_improve_init_tables(&si);
                        (void)hu_self_improve_get_prompt_patches(&si, &patches, &patches_len);
                        hu_self_improve_deinit(&si);
                    }

                    size_t ctx_len =
                        digest_len + (trend_section ? trend_len : 0) + (patches ? patches_len : 0);
                    char *ctx_buf = (char *)alloc->alloc(alloc->ctx, ctx_len + 1);
                    if (ctx_buf) {
                        size_t p = 0;
                        if (patches && patches_len > 0) {
                            memcpy(ctx_buf + p, patches, patches_len);
                            p += patches_len;
                        }
                        if (trend_section) {
                            memcpy(ctx_buf + p, trend_section, trend_len);
                            p += trend_len;
                        }
                        memcpy(ctx_buf + p, digest, digest_len);
                        p += digest_len;
                        ctx_buf[p] = '\0';
                        if (hu_research_build_prompt(alloc, ctx_buf, p, &enriched_prompt,
                                                     &enriched_len) == HU_OK &&
                            enriched_prompt)
                            effective_prompt = enriched_prompt;
                        alloc->free(alloc->ctx, ctx_buf, ctx_len + 1);
                    }
                    if (patches)
                        alloc->free(alloc->ctx, patches, patches_len + 1);
                    if (trend_section)
                        alloc->free(alloc->ctx, trend_section, trend_len + 1);
                    alloc->free(alloc->ctx, digest, digest_len + 1);
                }
            }
        }
#endif

        char *response = NULL;
        size_t response_len = 0;
        err = hu_agent_run_single(agent_p, effective_prompt, strlen(effective_prompt), msg,
                                  strlen(msg), &response, &response_len);
        if (err == HU_OK && response && response_len > 0) {
            fwrite(response, 1, response_len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
            {
                sqlite3 *findings_db = hu_sqlite_memory_get_db(&memory);
                if (findings_db) {
                    hu_findings_parse_and_store(alloc, findings_db, response, response_len);
#if defined(HU_HAS_SKILLS)
                    hu_intelligence_cycle_result_t cycle_result;
                    memset(&cycle_result, 0, sizeof(cycle_result));
                    if (hu_intelligence_run_cycle(alloc, findings_db, &cycle_result) == HU_OK) {
                        hu_log_info("intelligence", NULL,
                                    "cycle: %zu actioned, %zu lessons, %zu events, %zu "
                                    "values",
                                    cycle_result.findings_actioned, cycle_result.lessons_extracted,
                                    cycle_result.events_recorded, cycle_result.values_learned);
                    }
#endif
                }
            }
#endif
            alloc->free(alloc->ctx, response, response_len + 1);
        } else if (err != HU_OK) {
            hu_log_error("human", &observer, "agent turn failed: %s", hu_error_string(err));
#if defined(HU_ENABLE_FEEDS) && defined(HU_ENABLE_SQLITE)
            {
                sqlite3 *fail_db = hu_sqlite_memory_get_db(&memory);
                if (fail_db) {
                    sqlite3_stmt *fb = NULL;
                    if (sqlite3_prepare_v2(
                            fail_db,
                            "INSERT INTO behavioral_feedback "
                            "(behavior_type, contact_id, signal, context, timestamp) "
                            "VALUES ('research_cycle', 'system', 'negative', ?, ?)",
                            -1, &fb, NULL) == SQLITE_OK) {
                        char ctx_buf[128];
                        int cl = snprintf(ctx_buf, sizeof(ctx_buf), "Agent failed: %s",
                                          hu_error_string(err));
                        if (cl < 0) {
                            cl = 0;
                        }
                        if ((size_t)cl >= sizeof(ctx_buf)) {
                            cl = (int)(sizeof(ctx_buf) - 1);
                        }
                        sqlite3_bind_text(fb, 1, ctx_buf, cl, SQLITE_STATIC);
                        sqlite3_bind_int64(fb, 2, (int64_t)time(NULL));
                        sqlite3_step(fb);
                        sqlite3_finalize(fb);
                    }
                    hu_online_learning_t ol = {0};
                    if (hu_online_learning_create(alloc, fail_db, 0.1, &ol) == HU_OK) {
                        (void)hu_online_learning_init_tables(&ol);
                        int64_t now = (int64_t)time(NULL);
                        (void)hu_online_learning_update_weight(&ol, "research_findings", 18, -0.5,
                                                               now);
                        hu_online_learning_deinit(&ol);
                    }
                }
            }
#endif
        }
        if (enriched_prompt)
            alloc->free(alloc->ctx, enriched_prompt, enriched_len + 1);
        hu_agent_deinit(agent_p);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
#ifdef HU_ENABLE_SQLITE
        if (cli_graph) {
            hu_memory_v1_graph_close(cli_graph, alloc);
            cli_graph = NULL;
        }
#endif
        if (vector_store.vtable && vector_store.vtable->deinit)
            vector_store.vtable->deinit(vector_store.ctx, alloc);
        if (embedder.vtable && embedder.vtable->deinit)
            embedder.vtable->deinit(embedder.ctx, alloc);
        if (memory.vtable && memory.vtable->deinit)
            memory.vtable->deinit(memory.ctx);
        if (observer.vtable && observer.vtable->deinit)
            observer.vtable->deinit(observer.ctx);
        if (log_fp)
            fclose(log_fp);
        hu_tools_destroy_default(alloc, tools, tools_count);
#ifdef HU_HAS_OTEL
        if (otel_observer.vtable && otel_observer.vtable->deinit)
            otel_observer.vtable->deinit(otel_observer.ctx);
#endif
        if (agent_p->policy_engine)
            hu_policy_engine_destroy(agent_p->policy_engine);
        if (cli_mailbox)
            hu_mailbox_destroy(cli_mailbox);
        if (cli_agent_pool)
            hu_agent_pool_destroy(cli_agent_pool);
        if (cron)
            hu_cron_destroy(cron, alloc);
        if (policy.tracker)
            hu_rate_tracker_destroy(policy.tracker);
        if (sb_storage)
            hu_sandbox_storage_destroy(sb_storage, &sb_alloc);
        hu_awareness_deinit(&cli_awareness);
        hu_bus_deinit(&cli_bus);
#ifdef HU_HAS_VOICE_CHANNEL
        hu_channel_voice_destroy(&cli_voice_ch);
#endif
        hu_config_deinit(&cfg);
        alloc->free(alloc->ctx, agent_p, sizeof(*agent_p));
        return err;
    }

    /* Install SIGINT handler */
    g_active_agent = agent_p;
    struct sigaction sa, old_sa, old_hup;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);

#if !defined(_WIN32)
    /* Install SIGHUP handler for config hot-reload */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, &old_hup);

    signal(SIGPIPE, SIG_IGN);
#endif

    int single_message_mode = (parsed_args.message && parsed_args.message[0]);
    int use_ansi = isatty(STDOUT_FILENO) && !single_message_mode;
    g_cli_use_ansi = use_ansi;

    if (!single_message_mode)
        print_banner(prov_name, model, tools_count);

    /* `-m` / `--message`: propagate turn errors to process exit (scripts, harness, CI smoke). */
    hu_error_t single_message_exit = HU_OK;

    int one_shot = single_message_mode;
    while (1) {
        /* Check for SIGHUP signal to reload config */
#ifndef _WIN32
        if (g_reload_requested) {
            g_reload_requested = 0;
            char *summary = NULL;
            size_t summary_len = 0;
            hu_error_t reload_err = hu_agent_reload_config(agent_p, &summary, &summary_len);
            if (reload_err == HU_OK && summary) {
                hu_log_info("human", &observer, "config reloaded via SIGHUP");
                hu_log_info("human", &observer, "%s", summary);
                alloc->free(alloc->ctx, summary, summary_len + 1);
            } else if (reload_err != HU_OK) {
                hu_log_error("human", &observer, "config reload failed: %s",
                             hu_error_string(reload_err));
            }
        }
#endif

        char *line = NULL;
        size_t line_len = 0;
        int line_owned = 1;

        if (one_shot) {
            line = (char *)parsed_args.message;
            line_len = strlen(parsed_args.message);
            line_owned = 0;
            one_shot = 0;
        } else {
            printf(HU_COLOR_BOLD HU_COLOR_SUCCESS "> " HU_COLOR_RESET);
            fflush(stdout);
            line = hu_cli_readline(alloc, &line_len);
        }
        if (!line)
            break;

        if (line_owned && hu_cli_is_quit_command(line, line_len)) {
            alloc->free(alloc->ctx, line, line_len + 1);
            break;
        }

        char *slash = hu_agent_handle_slash_command(agent_p, line, line_len);
        if (slash) {
            printf("%s\n", slash);
            alloc->free(alloc->ctx, slash, strlen(slash) + 1);
            if (line_owned)
                alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }

        g_cancel = 0;
        agent_p->cancel_requested = 0;
        cli_stream_started = 0;

        /* Adaptive model routing for CLI (Gemini slot names only for Google family providers) */
#ifndef HU_IS_TEST
        if (cli_provider_uses_gemini_slot_models(prov_name, prov_name_len)) {
            hu_model_router_config_t mr_cfg = hu_model_router_default_config();
            mr_cfg.conversation_floor = HU_TIER_CONVERSATIONAL;
            if (cfg.agent.mr_reflexive_model) {
                mr_cfg.reflexive_model = cfg.agent.mr_reflexive_model;
                mr_cfg.reflexive_model_len = strlen(cfg.agent.mr_reflexive_model);
            }
            if (cfg.agent.mr_conversational_model) {
                mr_cfg.conversational_model = cfg.agent.mr_conversational_model;
                mr_cfg.conversational_model_len = strlen(cfg.agent.mr_conversational_model);
            }
            if (cfg.agent.mr_analytical_model) {
                mr_cfg.analytical_model = cfg.agent.mr_analytical_model;
                mr_cfg.analytical_model_len = strlen(cfg.agent.mr_analytical_model);
            }
            if (cfg.agent.mr_deep_model) {
                mr_cfg.deep_model = cfg.agent.mr_deep_model;
                mr_cfg.deep_model_len = strlen(cfg.agent.mr_deep_model);
            }
            if (cfg.agent.mr_on_device_model) {
                mr_cfg.on_device_model = cfg.agent.mr_on_device_model;
                mr_cfg.on_device_model_len = strlen(cfg.agent.mr_on_device_model);
            }
#ifdef HU_ENABLE_APPLE_INTELLIGENCE
            if (cfg.agent.mr_on_device_enabled) {
                mr_cfg.on_device_available = hu_apple_probe(agent_p->alloc, NULL, 0);
            }
#endif
            time_t now_rt = time(NULL);
            struct tm *lt = localtime(&now_rt);
            int hour = lt ? lt->tm_hour : 12;
            hu_model_selection_t sel;
            /* One-shot `-m` is used by scripts and smoke tests; skip the judge
             * LLM round-trip (extra provider.chat_with_system) so completion
             * stays bounded. Interactive sessions keep full judge routing. */
            if (cfg.agent.mr_judge_enabled && !single_message_mode) {
                static hu_route_cache_t cli_judge_cache;
                static bool cli_judge_cache_inited = false;
                if (!cli_judge_cache_inited) {
                    hu_route_cache_init(&cli_judge_cache);
                    cli_judge_cache_inited = true;
                }
                const char *jm = mr_cfg.reflexive_model;
                size_t jm_len = mr_cfg.reflexive_model_len;
                if (cfg.agent.mr_judge_model) {
                    jm = cfg.agent.mr_judge_model;
                    jm_len = strlen(cfg.agent.mr_judge_model);
                }
                sel = hu_model_route_with_judge(&mr_cfg, line, line_len, NULL, 0, hour,
                                                agent_p->history_count, &agent_p->provider, jm,
                                                jm_len, agent_p->alloc, &cli_judge_cache);
            } else {
                sel =
                    hu_model_route(&mr_cfg, line, line_len, NULL, 0, hour, agent_p->history_count);
            }
            agent_p->turn_model = sel.model;
            agent_p->turn_model_len = sel.model_len;
            agent_p->turn_temperature = sel.temperature;
            agent_p->turn_thinking_budget = sel.thinking_budget;
            agent_p->turn_tier = (int)sel.tier;
        } else {
            agent_p->turn_model = agent_p->model_name;
            agent_p->turn_model_len = agent_p->model_name_len;
            agent_p->turn_temperature = agent_p->temperature;
            agent_p->turn_thinking_budget = 0;
            /* Still compute tier for tool gating even with non-Gemini providers */
            hu_model_router_config_t mr_cfg_nongem = hu_model_router_default_config();
            hu_model_selection_t sel_nongem =
                hu_model_route(&mr_cfg_nongem, line, line_len, NULL, 0, -1, agent_p->history_count);
            agent_p->turn_tier = (int)sel_nongem.tier;
        }
#endif

#if HU_CLI_ASYNC
        /* Heap-allocate the thread-context. Two reasons:
         *  1. Defense against the ASan stack-use-after-scope false positive on
         *     macOS arm64 + pthread: when T1 exits hu_agent_turn (whose frame
         *     is ~485 KB), pthread's lazy stack mmap-release leaves f8 poison
         *     in shadow memory. T0's later READ of tctx->done in the spinner
         *     loop can resolve into that stale-poisoned region and trip ASan
         *     even though the underlying memory is valid. Heap storage lives
         *     outside the contested pthread-stack range and ASan tracks it
         *     via the heap allocator hooks.
         *  2. Defense-in-depth against any genuine T0/T1 stack-region overlap
         *     that the platform allocator might produce in future macOS revs.
         * Cost is negligible: one alloc + one free per CLI turn.
         * Pinned by Chip G investigation, 2026-05-26 trusting-tharp session. */
        agent_turn_ctx_t *tctx = (agent_turn_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*tctx));
        if (!tctx) {
            hu_log_error("error", NULL, "failed to allocate agent turn context");
            if (line_owned)
                alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }
        memset(tctx, 0, sizeof(*tctx));
        tctx->agent = agent_p;
        tctx->msg = line;
        tctx->msg_len = line_len;

        pthread_t tid;
        if (pthread_create(&tid, NULL, agent_turn_thread, tctx) != 0) {
            hu_log_error("error", NULL, "failed to start agent thread");
            alloc->free(alloc->ctx, tctx, sizeof(*tctx));
            if (line_owned)
                alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }

        run_spinner_loop(tctx, use_ansi);

        if (g_cancel && g_active_agent)
            g_active_agent->cancel_requested = 1;

        if (g_cancel && !tctx->done) {
            if (use_ansi)
                printf(HU_ANSI_CLEAR_LINE HU_ANSI_SHOW_CURSOR HU_COLOR_WARNING
                       "Cancelled." HU_COLOR_RESET "\n");
            else
                printf("Cancelled.\n");
        }

        pthread_join(tid, NULL);
        err = tctx->err;
        if (single_message_mode)
            single_message_exit = err;

        if (err == HU_ERR_CANCELLED) {
            if (use_ansi)
                printf(HU_COLOR_DIM "Turn cancelled by user." HU_COLOR_RESET "\n");
            else
                printf("Turn cancelled by user.\n");
        } else if (err != HU_OK) {
            hu_log_error("error", NULL, "%s", hu_error_string(err));
        } else if (tctx->response && tctx->response_len > 0) {
            if (!cli_stream_started) {
                fwrite(tctx->response, 1, tctx->response_len, stdout);
            }
            fputc('\n', stdout);
            fflush(stdout);
            alloc->free(alloc->ctx, tctx->response, tctx->response_len + 1);
        }
        alloc->free(alloc->ctx, tctx, sizeof(*tctx));
#else
        char *response = NULL;
        size_t response_len = 0;
        agent_p->active_channel = "cli";
        agent_p->active_channel_len = 3;
        if (use_ansi)
            printf("Thinking...\r");
        fflush(stdout);
        err = hu_agent_turn_stream(agent_p, line, line_len, cli_stream_token, NULL, &response,
                                   &response_len);
        if (single_message_mode)
            single_message_exit = err;
        if (use_ansi && !cli_stream_started) {
            printf("                    \r");
            fflush(stdout);
        }
        if (err == HU_ERR_CANCELLED) {
            printf("Turn cancelled.\n");
        } else if (err != HU_OK) {
            hu_log_error("error", NULL, "%s", hu_error_string(err));
        } else if (response && response_len > 0) {
            fputc('\n', stdout);
            fflush(stdout);
            alloc->free(alloc->ctx, response, response_len + 1);
        }
#endif

        if (line_owned)
            alloc->free(alloc->ctx, line, line_len + 1);
        if (!line_owned)
            break;
    }

    if (!single_message_mode)
        printf("\n" HU_COLOR_DIM "Goodbye." HU_COLOR_RESET "\n");
    g_active_agent = NULL;
    sigaction(SIGINT, &old_sa, NULL);
#ifndef _WIN32
    sigaction(SIGHUP, &old_hup, NULL);
#endif
    hu_agent_deinit(agent_p);
    if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
        retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
#ifdef HU_ENABLE_SQLITE
    if (cli_graph) {
        hu_memory_v1_graph_close(cli_graph, alloc);
        cli_graph = NULL;
    }
#endif
    if (vector_store.vtable && vector_store.vtable->deinit)
        vector_store.vtable->deinit(vector_store.ctx, alloc);
    if (embedder.vtable && embedder.vtable->deinit)
        embedder.vtable->deinit(embedder.ctx, alloc);
    if (memory.vtable && memory.vtable->deinit)
        memory.vtable->deinit(memory.ctx);
    if (observer.vtable && observer.vtable->deinit)
        observer.vtable->deinit(observer.ctx);
    if (log_fp)
        fclose(log_fp);
    hu_tools_destroy_default(alloc, tools, tools_count);
#ifdef HU_HAS_OTEL
    if (otel_observer.vtable && otel_observer.vtable->deinit)
        otel_observer.vtable->deinit(otel_observer.ctx);
#endif
    if (agent_p->policy_engine)
        hu_policy_engine_destroy(agent_p->policy_engine);
    if (cli_mailbox)
        hu_mailbox_destroy(cli_mailbox);
    if (cli_agent_pool)
        hu_agent_pool_destroy(cli_agent_pool);
    if (cron)
        hu_cron_destroy(cron, alloc);
    if (policy.tracker)
        hu_rate_tracker_destroy(policy.tracker);
    if (sb_storage)
        hu_sandbox_storage_destroy(sb_storage, &sb_alloc);
    hu_awareness_deinit(&cli_awareness);
    hu_bus_deinit(&cli_bus);
#ifdef HU_HAS_VOICE_CHANNEL
    hu_channel_voice_destroy(&cli_voice_ch);
#endif
    hu_config_deinit(&cfg);
    /* Single free site for both return paths — hoisted before the
     * conditional so we don't leak on the single_message_exit branch
     * and don't duplicate. */
    alloc->free(alloc->ctx, agent_p, sizeof(*agent_p));
    if (single_message_mode && single_message_exit != HU_OK)
        return single_message_exit;
    return HU_OK;
}
