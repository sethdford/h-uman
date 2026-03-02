#include "seaclaw/agent/cli.h"
#include <stdint.h>
#include "seaclaw/agent/tui.h"
#include "seaclaw/config.h"
#include "seaclaw/agent.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/runtime.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/security.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/channels/cli.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#if defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#define SC_CLI_ASYNC 1
#else
#define SC_CLI_ASYNC 0
#endif

#define SC_CODENAME "SeaClaw"
#define SC_CLI_MAX_PATH 1024

/* ── ANSI escape helpers ─────────────────────────────────────────────── */
#define SC_ANSI_RESET   "\033[0m"
#define SC_ANSI_BOLD    "\033[1m"
#define SC_ANSI_DIM     "\033[2m"
#define SC_ANSI_CYAN    "\033[36m"
#define SC_ANSI_GREEN   "\033[32m"
#define SC_ANSI_YELLOW  "\033[33m"
#define SC_ANSI_HIDE_CURSOR "\033[?25l"
#define SC_ANSI_SHOW_CURSOR "\033[?25h"
#define SC_ANSI_CLEAR_LINE  "\033[2K\r"

#if SC_CLI_ASYNC
static const char *spinner_frames[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
    "\xe2\xa0\x87", "\xe2\xa0\x8f"
};
#define SPINNER_FRAME_COUNT 10
#endif

/* ── Global cancel flag (set by SIGINT handler) ──────────────────────── */
static volatile sig_atomic_t g_cancel = 0;
static sc_agent_t *g_active_agent = NULL;

static void sigint_handler(int sig) {
    (void)sig;
    g_cancel = 1;
    if (g_active_agent)
        g_active_agent->cancel_requested = 1;
}

/* ── Memory from config ──────────────────────────────────────────────── */
static sc_memory_t create_memory_from_config(sc_allocator_t *alloc,
    const sc_config_t *cfg, const char *ws)
{
    const char *backend = cfg->memory.backend;
    if (!backend) backend = "markdown";

    if (strcmp(backend, "sqlite") == 0) {
        const char *path = cfg->memory.sqlite_path;
        char buf[SC_CLI_MAX_PATH];
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

    if (strcmp(backend, "none") == 0)
        return sc_none_memory_create(alloc);

    if (strcmp(backend, "lru") == 0 || strcmp(backend, "memory") == 0)
        return sc_memory_lru_create(alloc, 256);

    if (strcmp(backend, "lancedb") == 0) {
        char buf2[SC_CLI_MAX_PATH];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.seaclaw/lancedb", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return sc_lancedb_memory_create(alloc, buf2);
        }
        return sc_lancedb_memory_create(alloc, ".seaclaw/lancedb");
    }

    if (strcmp(backend, "lucid") == 0) {
        char buf2[SC_CLI_MAX_PATH];
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

/* ── Arg parsing ─────────────────────────────────────────────────────── */
sc_error_t sc_agent_cli_parse_args(const char *const *argv, size_t argc,
    sc_parsed_agent_args_t *out) {
    if (!argv || !out) return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if (strcmp(a, "-m") == 0 || strcmp(a, "--message") == 0) {
            if (i + 1 < argc) { out->message = argv[i + 1]; i++; }
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--session") == 0) {
            if (i + 1 < argc) { out->session_id = argv[i + 1]; i++; }
        } else if (strcmp(a, "--provider") == 0) {
            if (i + 1 < argc) { out->provider_override = argv[i + 1]; i++; }
        } else if (strcmp(a, "--model") == 0) {
            if (i + 1 < argc) { out->model_override = argv[i + 1]; i++; }
        } else if (strcmp(a, "--temperature") == 0) {
            if (i + 1 < argc) {
                out->temperature_override = 0.0;
                sscanf(argv[i + 1], "%lf", &out->temperature_override);
                out->has_temperature = 1;
                i++;
            }
        } else if (strcmp(a, "--tui") == 0) {
            out->use_tui = 1;
        }
    }
    return SC_OK;
}

/* ── Streaming token callback ────────────────────────────────────────── */
static volatile sig_atomic_t cli_stream_started = 0;

static void cli_stream_token(const char *delta, size_t len, void *ctx) {
    (void)ctx;
    if (delta && len > 0) {
        if (!cli_stream_started) {
            cli_stream_started = 1;
            printf(SC_ANSI_SHOW_CURSOR SC_ANSI_CLEAR_LINE);
        }
        fwrite(delta, 1, len, stdout);
        fflush(stdout);
    }
}

/* ── Background agent turn (async mode) ──────────────────────────────── */
#if SC_CLI_ASYNC
typedef struct agent_turn_ctx {
    sc_agent_t *agent;
    const char *msg;
    size_t msg_len;
    char *response;
    size_t response_len;
    sc_error_t err;
    volatile int done;
} agent_turn_ctx_t;

static void *agent_turn_thread(void *arg) {
    agent_turn_ctx_t *ctx = (agent_turn_ctx_t *)arg;
    ctx->err = sc_agent_turn_stream(ctx->agent, ctx->msg, ctx->msg_len,
        cli_stream_token, NULL, &ctx->response, &ctx->response_len);
    ctx->done = 1;
    return NULL;
}

static int get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static void run_spinner_loop(agent_turn_ctx_t *tctx) {
    int frame = 0;
    printf(SC_ANSI_HIDE_CURSOR);
    fflush(stdout);

    while (!tctx->done && !g_cancel) {
        if (!cli_stream_started) {
            int width = get_terminal_width();
            const char *label = " Thinking...";
            printf(SC_ANSI_CLEAR_LINE SC_ANSI_CYAN "%s" SC_ANSI_RESET
                   SC_ANSI_DIM "%s" SC_ANSI_RESET,
                spinner_frames[frame % SPINNER_FRAME_COUNT], label);
            (void)width;
            fflush(stdout);
            frame++;
        }

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        poll(&pfd, 1, 80);
    }

    if (!cli_stream_started) {
        printf(SC_ANSI_CLEAR_LINE SC_ANSI_SHOW_CURSOR);
        fflush(stdout);
    } else {
        printf(SC_ANSI_SHOW_CURSOR);
        fflush(stdout);
    }
}
#endif /* SC_CLI_ASYNC */

/* ── Print welcome banner ────────────────────────────────────────────── */
static void print_banner(const char *prov_name, const char *model, size_t tools_count) {
    printf(SC_ANSI_BOLD SC_ANSI_CYAN "%s" SC_ANSI_RESET " v%s\n",
        SC_CODENAME, sc_version_string());
    printf(SC_ANSI_DIM "Provider: %s | Model: %s | Tools: %zu" SC_ANSI_RESET "\n",
        prov_name, (model[0] ? model : "(default)"), tools_count);
    printf("Type your message, or " SC_ANSI_DIM "'exit'" SC_ANSI_RESET
           " to leave. " SC_ANSI_DIM "Ctrl+C cancels a running turn." SC_ANSI_RESET "\n\n");
}

/* ── Main CLI loop ───────────────────────────────────────────────────── */
sc_error_t sc_agent_cli_run(sc_allocator_t *alloc, const char *const *argv, size_t argc) {
    if (!alloc) return SC_ERR_INVALID_ARGUMENT;

    sc_parsed_agent_args_t parsed_args;
    sc_agent_cli_parse_args(argv, argc, &parsed_args);

    sc_config_t cfg;
    sc_error_t err = sc_config_load(alloc, &cfg);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Config error: %s\n", SC_CODENAME, sc_error_string(err));
        return err;
    }

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

    const char *model = cfg.default_model ? cfg.default_model : "";
    const char *ws = cfg.workspace_dir ? cfg.workspace_dir : ".";
    double temp = cfg.temperature > 0.0 ? cfg.temperature : 0.7;
    uint32_t max_iters = cfg.agent.max_tool_iterations > 0 ? cfg.agent.max_tool_iterations : 25;
    uint32_t max_hist = cfg.agent.max_history_messages > 0 ? cfg.agent.max_history_messages : 100;

    sc_runtime_t runtime;
    err = sc_runtime_from_config(&cfg, &runtime);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Runtime '%s' not supported: %s\n", SC_CODENAME,
            cfg.runtime.kind ? cfg.runtime.kind : "(null)", sc_error_string(err));
        sc_config_deinit(&cfg);
        return err;
    }

    sc_security_policy_t policy = {0};
    policy.autonomy = (sc_autonomy_level_t)cfg.security.autonomy_level;
    policy.workspace_dir = ws;
    policy.workspace_only = true;
    policy.allow_shell = (policy.autonomy != SC_AUTONOMY_READ_ONLY);
    if (runtime.vtable && runtime.vtable->has_shell_access)
        policy.allow_shell = policy.allow_shell && runtime.vtable->has_shell_access(runtime.ctx);
    policy.block_high_risk_commands = (policy.autonomy == SC_AUTONOMY_SUPERVISED);
    policy.require_approval_for_medium_risk = (policy.autonomy == SC_AUTONOMY_SUPERVISED);
    policy.max_actions_per_hour = 100;
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

    sc_observer_t observer = {0};
    FILE *log_fp = NULL;
    const char *log_env = getenv("SEACLAW_LOG");
    if (log_env && log_env[0]) {
        log_fp = fopen(log_env, "a");
        if (log_fp)
            observer = sc_log_observer_create(alloc, log_fp);
    }

    sc_memory_t memory = create_memory_from_config(alloc, &cfg, ws);
    sc_session_store_t session_store = {0};
    if (memory.vtable && strcmp(cfg.memory.backend ? cfg.memory.backend : "markdown", "sqlite") == 0)
        session_store = sc_sqlite_memory_get_session_store(&memory);

    sc_tool_t *tools = NULL;
    size_t tools_count = 0;
    err = sc_tools_create_default(alloc, ws, strlen(ws), &policy, &cfg,
        memory.vtable ? &memory : NULL, &tools, &tools_count);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Tools init failed: %s\n", SC_CODENAME, sc_error_string(err));
        if (memory.vtable && memory.vtable->deinit) memory.vtable->deinit(memory.ctx);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
        sc_config_deinit(&cfg);
        return err;
    }

    sc_agent_t agent;
    err = sc_agent_from_config(&agent, alloc, provider,
        tools, tools_count,
        memory.vtable ? &memory : NULL,
        session_store.vtable ? &session_store : NULL,
        &observer, NULL,
        model, strlen(model),
        prov_name, prov_name_len,
        temp, ws, strlen(ws), max_iters, max_hist, cfg.memory.auto_save,
        2, NULL, 0);
    if (err != SC_OK) {
        fprintf(stderr, "[%s] Agent init failed: %s\n", SC_CODENAME, sc_error_string(err));
        if (observer.vtable && observer.vtable->deinit)
            observer.vtable->deinit(observer.ctx);
        if (log_fp) fclose(log_fp);
        sc_tools_destroy_default(alloc, tools, tools_count);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
        sc_config_deinit(&cfg);
        return err;
    }

    /* TUI mode: launch split-pane terminal UI if --tui was passed */
    if (parsed_args.use_tui) {
        sc_tui_state_t tui_state;
        err = sc_tui_init(&tui_state, alloc, &agent, prov_name, model, tools_count);
        if (err == SC_OK) {
            err = sc_tui_run(&tui_state);
            sc_tui_deinit(&tui_state);
        } else {
            fprintf(stderr, "[%s] TUI not available: %s\n", SC_CODENAME,
                sc_error_string(err));
        }
        sc_agent_deinit(&agent);
        if (memory.vtable && memory.vtable->deinit) memory.vtable->deinit(memory.ctx);
        if (observer.vtable && observer.vtable->deinit) observer.vtable->deinit(observer.ctx);
        if (log_fp) fclose(log_fp);
        sc_tools_destroy_default(alloc, tools, tools_count);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
        sc_config_deinit(&cfg);
        return err;
    }

    /* Install SIGINT handler */
    g_active_agent = &agent;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    print_banner(prov_name, model, tools_count);

    while (1) {
        printf(SC_ANSI_BOLD SC_ANSI_GREEN "> " SC_ANSI_RESET);
        fflush(stdout);

        size_t line_len = 0;
        char *line = sc_cli_readline(alloc, &line_len);
        if (!line) break;

        if (sc_cli_is_quit_command(line, line_len)) {
            alloc->free(alloc->ctx, line, line_len + 1);
            break;
        }

        char *slash = sc_agent_handle_slash_command(&agent, line, line_len);
        if (slash) {
            printf("%s\n", slash);
            alloc->free(alloc->ctx, slash, strlen(slash) + 1);
            alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }

        g_cancel = 0;
        agent.cancel_requested = 0;
        cli_stream_started = 0;

#if SC_CLI_ASYNC
        agent_turn_ctx_t tctx;
        memset(&tctx, 0, sizeof(tctx));
        tctx.agent = &agent;
        tctx.msg = line;
        tctx.msg_len = line_len;

        pthread_t tid;
        if (pthread_create(&tid, NULL, agent_turn_thread, &tctx) != 0) {
            fprintf(stderr, "[error] failed to start agent thread\n");
            alloc->free(alloc->ctx, line, line_len + 1);
            continue;
        }

        run_spinner_loop(&tctx);

        if (g_cancel && !tctx.done) {
            printf(SC_ANSI_CLEAR_LINE SC_ANSI_SHOW_CURSOR
                   SC_ANSI_YELLOW "Cancelled." SC_ANSI_RESET "\n");
        }

        pthread_join(tid, NULL);
        err = tctx.err;

        if (err == SC_ERR_CANCELLED) {
            printf(SC_ANSI_DIM "Turn cancelled by user." SC_ANSI_RESET "\n");
        } else if (err != SC_OK) {
            fprintf(stderr, "[error] %s\n", sc_error_string(err));
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
        printf("Thinking...\r");
        fflush(stdout);
        err = sc_agent_turn_stream(&agent, line, line_len,
            cli_stream_token, NULL, &response, &response_len);
        if (!cli_stream_started) {
            printf("                    \r");
            fflush(stdout);
        }
        if (err == SC_ERR_CANCELLED) {
            printf("Turn cancelled.\n");
        } else if (err != SC_OK) {
            fprintf(stderr, "[error] %s\n", sc_error_string(err));
        } else if (response && response_len > 0) {
            fputc('\n', stdout);
            fflush(stdout);
            alloc->free(alloc->ctx, response, response_len + 1);
        }
#endif

        alloc->free(alloc->ctx, line, line_len + 1);
    }

    printf("\n" SC_ANSI_DIM "Goodbye." SC_ANSI_RESET "\n");
    g_active_agent = NULL;
    sc_agent_deinit(&agent);
    if (memory.vtable && memory.vtable->deinit)
        memory.vtable->deinit(memory.ctx);
    if (observer.vtable && observer.vtable->deinit)
        observer.vtable->deinit(observer.ctx);
    if (log_fp) fclose(log_fp);
    sc_tools_destroy_default(alloc, tools, tools_count);
    if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
    if (sb_storage) sc_sandbox_storage_destroy(sb_storage, &sb_alloc);
    sc_config_deinit(&cfg);
    return SC_OK;
}
