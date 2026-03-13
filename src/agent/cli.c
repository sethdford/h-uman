#include "human/agent/cli.h"
#include "human/agent.h"
#include "human/agent/awareness.h"
#include "human/agent/outcomes.h"
#include "human/agent/profile.h"
#include "human/agent/tui.h"
#include "human/channels/cli.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/string.h"
#ifdef HU_HAS_CRON
#include "human/cron.h"
#endif
#include "human/bus.h"
#include "human/design_tokens.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/memory/factory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/observability/log_observer.h"
#ifdef HU_HAS_OTEL
#include "human/observability/otel.h"
#endif
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
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* ── Global cancel flag (set by SIGINT handler) ──────────────────────── */
static volatile sig_atomic_t g_cancel = 0;
static hu_agent_t *g_active_agent = NULL;

static void sigint_handler(int sig) {
    (void)sig;
    g_cancel = 1;
    if (g_active_agent)
        g_active_agent->cancel_requested = 1;
}

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
        if (strcmp(a, "-m") == 0 || strcmp(a, "--message") == 0) {
            if (i + 1 < argc) {
                out->message = argv[i + 1];
                i++;
            }
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--session") == 0) {
            if (i + 1 < argc) {
                out->session_id = argv[i + 1];
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
    printf(HU_COLOR_BOLD HU_COLOR_ACCENT "%s" HU_COLOR_RESET " v%s\n", HU_CODENAME,
           hu_version_string());
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

    hu_config_t cfg;
    hu_error_t err = hu_config_load(alloc, &cfg);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", HU_CODENAME, hu_error_string(err));
        return err;
    }

    if (parsed_args.demo_mode) {
        fprintf(stderr, "[human] Demo mode — using local Ollama (no API key required)\n");
        fprintf(stderr, "[human] Make sure Ollama is running: ollama serve\n");
        cfg.default_provider = "ollama";
        cfg.default_model = "llama3.2";
        cfg.memory.backend = "none";
        cfg.memory_backend = "none";
    }

    const char *prov_name = cfg.default_provider ? cfg.default_provider : "openai";
    size_t prov_name_len = strlen(prov_name);
    const char *api_key = hu_config_default_provider_key(&cfg);
    size_t api_key_len = api_key ? strlen(api_key) : 0;
    const char *base_url = hu_config_get_provider_base_url(&cfg, prov_name);
    size_t base_url_len = base_url ? strlen(base_url) : 0;

    hu_provider_t provider;
    err = hu_provider_create(alloc, prov_name, prov_name_len, api_key, api_key_len, base_url,
                             base_url_len, &provider);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Provider '%s' init failed: %s\n", HU_CODENAME, prov_name,
                hu_error_string(err));
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
        fprintf(stderr, "[%s] Runtime '%s' not supported: %s\n", HU_CODENAME,
                cfg.runtime.kind ? cfg.runtime.kind : "(null)", hu_error_string(err));
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
            fprintf(stderr, "[warn] could not open log file '%s': %s\n", log_env, strerror(errno));
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

#ifdef HU_HAS_CRON
    hu_cron_scheduler_t *cron = hu_cron_create(alloc, 64, true);
#else
    hu_cron_scheduler_t *cron = NULL;
#endif

    hu_agent_pool_t *cli_agent_pool = hu_agent_pool_create(alloc, cfg.agent.pool_max_concurrent);
    hu_mailbox_t *cli_mailbox = hu_mailbox_create(alloc, 64);

    hu_tool_t *tools = NULL;
    size_t tools_count = 0;
    err = hu_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,
                                  memory.vtable ? &memory : NULL, cron, cli_agent_pool, cli_mailbox,
                                  NULL, NULL, &tools, &tools_count);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Tools init failed: %s\n", HU_CODENAME, hu_error_string(err));
        if (cron)
            hu_cron_destroy(cron, alloc);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
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
        hu_config_deinit(&cfg);
        return err;
    }

    hu_agent_context_config_t ctx_cfg = {
        .token_limit = cfg.agent.token_limit,
        .pressure_warn = cfg.agent.context_pressure_warn,
        .pressure_compact = cfg.agent.context_pressure_compact,
        .compact_target = cfg.agent.context_compact_target,
        .llm_compiler_enabled = cfg.agent.llm_compiler_enabled,
        .tree_of_thought = cfg.agent.tree_of_thought,
        .constitutional_ai = cfg.agent.constitutional_ai,
        .speculative_cache = cfg.agent.speculative_cache,
        .tool_routing_enabled = cfg.agent.tool_routing_enabled,
    };
    hu_agent_t agent;
    err = hu_agent_from_config(
        &agent, alloc, provider, tools, tools_count, memory.vtable ? &memory : NULL,
        session_store.vtable ? &session_store : NULL, &observer, NULL, model, strlen(model),
        prov_name, prov_name_len, temp, ws, strlen(ws), max_iters, max_hist, cfg.memory.auto_save,
        2, NULL, 0, cfg.agent.persona, cfg.agent.persona ? strlen(cfg.agent.persona) : 0, &ctx_cfg);
    if (err != HU_OK) {
        fprintf(stderr, "[%s] Agent init failed: %s\n", HU_CODENAME, hu_error_string(err));
        if (observer.vtable && observer.vtable->deinit)
            observer.vtable->deinit(observer.ctx);
        if (log_fp)
            fclose(log_fp);
        hu_tools_destroy_default(alloc, tools, tools_count);
        if (cron)
            hu_cron_destroy(cron, alloc);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
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
        hu_config_deinit(&cfg);
        return err;
    }
    agent.agent_pool = cli_agent_pool;
    hu_agent_set_mailbox(&agent, cli_mailbox);
    agent.policy_engine = NULL;

    if (cfg.security.audit.enabled) {
        hu_audit_config_t acfg = HU_AUDIT_CONFIG_DEFAULT;
        acfg.enabled = true;
        acfg.log_path = cfg.security.audit.log_path ? cfg.security.audit.log_path : "audit.log";
        acfg.max_size_mb = cfg.security.audit.max_size_mb > 0 ? cfg.security.audit.max_size_mb : 10;
        agent.audit_logger = hu_audit_logger_create(alloc, &acfg, ws);
    }

    if (cfg.policy.enabled) {
        agent.policy_engine = hu_policy_engine_create(alloc);
    }

    if (cfg.agent.default_profile) {
        const hu_agent_profile_t *prof =
            hu_agent_profile_by_name(cfg.agent.default_profile, strlen(cfg.agent.default_profile));
        if (prof) {
            if (prof->preferred_model && prof->preferred_model[0] && !parsed_args.model_override) {
                char *old = agent.model_name;
                size_t old_len = agent.model_name_len;
                agent.model_name =
                    hu_strndup(alloc, prof->preferred_model, strlen(prof->preferred_model));
                agent.model_name_len = strlen(prof->preferred_model);
                if (old)
                    alloc->free(alloc->ctx, old, old_len + 1);
            }
            if (prof->temperature > 0)
                agent.temperature = prof->temperature;
            if (prof->max_iterations > 0)
                agent.max_tool_iterations = prof->max_iterations;
            if (prof->max_history > 0)
                agent.max_history_messages = prof->max_history;
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
            agent.observer = &otel_observer;
        }
    }
#endif

    hu_agent_set_retrieval_engine(&agent, &retrieval_engine);
    if (cli_awareness.bus)
        hu_agent_set_awareness(&agent, (struct hu_awareness *)&cli_awareness);

    hu_outcome_tracker_t cli_outcomes;
    hu_outcome_tracker_init(&cli_outcomes, true);
    hu_agent_set_outcomes(&agent, &cli_outcomes);
    agent.scheduler = (struct hu_cron_scheduler *)cron;

    /* TUI mode: launch split-pane terminal UI if --tui was passed */
    if (parsed_args.use_tui) {
        hu_tui_state_t tui_state;
        err = hu_tui_init(&tui_state, alloc, &agent, prov_name, model, tools_count);
        if (err == HU_OK) {
            err = hu_tui_run(&tui_state);
            hu_tui_deinit(&tui_state);
        } else {
            fprintf(stderr, "[%s] TUI not available: %s\n", HU_CODENAME, hu_error_string(err));
        }
        hu_agent_deinit(&agent);
        if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
            retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
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
        if (agent.policy_engine)
            hu_policy_engine_destroy(agent.policy_engine);
        if (cli_mailbox)
            hu_mailbox_destroy(cli_mailbox);
        if (cli_agent_pool)
            hu_agent_pool_destroy(cli_agent_pool);
        if (policy.tracker)
            hu_rate_tracker_destroy(policy.tracker);
        if (sb_storage)
            hu_sandbox_storage_destroy(sb_storage, &sb_alloc);
        hu_awareness_deinit(&cli_awareness);
        hu_config_deinit(&cfg);
        return err;
    }

    /* Install SIGINT handler */
    g_active_agent = &agent;
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);

#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif

    int single_message_mode = (parsed_args.message && parsed_args.message[0]);
    int use_ansi = isatty(STDOUT_FILENO) && !single_message_mode;
    g_cli_use_ansi = use_ansi;

    if (!single_message_mode)
        print_banner(prov_name, model, tools_count);

    int one_shot = single_message_mode;
    while (1) {
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

        char *slash = hu_agent_handle_slash_command(&agent, line, line_len);
        if (slash) {
            printf("%s\n", slash);
            alloc->free(alloc->ctx, slash, strlen(slash) + 1);
            if (line_owned)
                alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }

        g_cancel = 0;
        agent.cancel_requested = 0;
        cli_stream_started = 0;

#if HU_CLI_ASYNC
        agent_turn_ctx_t tctx;
        memset(&tctx, 0, sizeof(tctx));
        tctx.agent = &agent;
        tctx.msg = line;
        tctx.msg_len = line_len;

        pthread_t tid;
        if (pthread_create(&tid, NULL, agent_turn_thread, &tctx) != 0) {
            fprintf(stderr, "[error] failed to start agent thread\n");
            if (line_owned)
                alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }

        run_spinner_loop(&tctx, use_ansi);

        if (g_cancel && !tctx.done) {
            if (use_ansi)
                printf(HU_ANSI_CLEAR_LINE HU_ANSI_SHOW_CURSOR HU_COLOR_WARNING
                       "Cancelled." HU_COLOR_RESET "\n");
            else
                printf("Cancelled.\n");
        }

        pthread_join(tid, NULL);
        err = tctx.err;

        if (err == HU_ERR_CANCELLED) {
            if (use_ansi)
                printf(HU_COLOR_DIM "Turn cancelled by user." HU_COLOR_RESET "\n");
            else
                printf("Turn cancelled by user.\n");
        } else if (err != HU_OK) {
            fprintf(stderr, "[error] %s\n", hu_error_string(err));
        } else if (tctx.response && tctx.response_len > 0) {
            if (!cli_stream_started) {
                fwrite(tctx.response, 1, tctx.response_len, stdout);
            }
            fputc('\n', stdout);
            fflush(stdout);
            alloc->free(alloc->ctx, tctx.response, tctx.response_len + 1);
        }
#else
        char *response = NULL;
        size_t response_len = 0;
        agent.active_channel = "cli";
        agent.active_channel_len = 3;
        if (use_ansi)
            printf("Thinking...\r");
        fflush(stdout);
        err = hu_agent_turn_stream(&agent, line, line_len, cli_stream_token, NULL, &response,
                                   &response_len);
        if (use_ansi && !cli_stream_started) {
            printf("                    \r");
            fflush(stdout);
        }
        if (err == HU_ERR_CANCELLED) {
            printf("Turn cancelled.\n");
        } else if (err != HU_OK) {
            fprintf(stderr, "[error] %s\n", hu_error_string(err));
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
    hu_agent_deinit(&agent);
    if (retrieval_engine.vtable && retrieval_engine.vtable->deinit)
        retrieval_engine.vtable->deinit(retrieval_engine.ctx, alloc);
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
    if (agent.policy_engine)
        hu_policy_engine_destroy(agent.policy_engine);
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
    hu_config_deinit(&cfg);
    return HU_OK;
}
