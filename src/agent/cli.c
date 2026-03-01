#include "seaclaw/agent/cli.h"
#include "seaclaw/config.h"
#include "seaclaw/agent.h"
#include "seaclaw/provider.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/runtime.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/security.h"
#include "seaclaw/observability/log_observer.h"
#include "seaclaw/channels/cli.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_CODENAME "SeaClaw"
#define SC_CLI_MAX_PATH 1024

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

    return sc_markdown_memory_create(alloc, ws);
}

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
        }
    }
    return SC_OK;
}

static void cli_stream_token(const char *delta, size_t len, void *ctx) {
    (void)ctx;
    if (delta && len > 0) {
        fwrite(delta, 1, len, stdout);
        fflush(stdout);
    }
}

sc_error_t sc_agent_cli_run(sc_allocator_t *alloc, const char *const *argv, size_t argc) {
    (void)argv;
    (void)argc;
    if (!alloc) return SC_ERR_INVALID_ARGUMENT;

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

    const char *model = cfg.default_model ? cfg.default_model : "gpt-4o";
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

    sc_observer_t observer = sc_log_observer_create(alloc, stderr);

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
        sc_tools_destroy_default(alloc, tools, tools_count);
        if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
        sc_config_deinit(&cfg);
        return err;
    }

    printf("%s v%s — Interactive Agent\n", SC_CODENAME, sc_version_string());
    printf("Provider: %s | Model: %s | Tools: %zu\n", prov_name, model, tools_count);
    printf("Type your message, or 'exit'/'quit' to leave.\n\n");

    while (1) {
        printf("> ");
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

        char *response = NULL;
        size_t response_len = 0;
        err = sc_agent_turn_stream(&agent, line, line_len,
            cli_stream_token, NULL, &response, &response_len);
        if (err != SC_OK) {
            fprintf(stderr, "[error] %s\n", sc_error_string(err));
        } else if (response && response_len > 0) {
            fputc('\n', stdout);
            fflush(stdout);
            alloc->free(alloc->ctx, response, response_len + 1);
        }

        alloc->free(alloc->ctx, line, line_len + 1);
    }

    printf("\nGoodbye.\n");
    sc_agent_deinit(&agent);
    if (memory.vtable && memory.vtable->deinit)
        memory.vtable->deinit(memory.ctx);
    if (observer.vtable && observer.vtable->deinit)
        observer.vtable->deinit(observer.ctx);
    sc_tools_destroy_default(alloc, tools, tools_count);
    if (policy.tracker) sc_rate_tracker_destroy(policy.tracker);
    sc_config_deinit(&cfg);
    return SC_OK;
}
