/* SeaClaw WASI entrypoint — full agent loop for WASM target.
 * Limitations: no shell tool, no process spawning, config from WASI fs or defaults. */
#ifdef __wasi__

#include <stdio.h>
#include <string.h>
#include "seaclaw/core/error.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/agent.h"
#include "seaclaw/config.h"
#include "seaclaw/wasm/wasm_alloc.h"
#include "seaclaw/wasm/wasm_provider.h"
#include "seaclaw/wasm/wasm_channel.h"
#include "seaclaw/wasm/wasi_bindings.h"

#define SC_VERSION "0.1.0"
#define SC_CODENAME "SeaClaw"
#define SC_WASI_PREOPEN_ROOT 3

static void print_usage(void) {
    const char *msg = "Usage: seaclaw agent [options]\n"
        "  agent  Start interactive CLI (stdin/stdout)\n"
        "  -h     Show help\n";
    size_t n = 0;
    sc_wasi_fd_write(1, msg, strlen(msg), &n);
}

static void print_version(void) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s v%s (WASI)\n", SC_CODENAME, SC_VERSION);
    if (len > 0) {
        size_t n = 0;
        sc_wasi_fd_write(1, buf, (size_t)len, &n);
    }
}

static int run_agent_loop(sc_allocator_t *alloc) {
    sc_provider_t provider;
    sc_channel_t channel;
    sc_agent_t agent;

    const char *api_key = getenv("OPENAI_API_KEY");
    size_t api_key_len = api_key ? strlen(api_key) : 0;
    sc_error_t err = sc_wasm_provider_create(alloc,
        api_key, api_key_len, NULL, 0, &provider);
    if (err != SC_OK) {
        const char *msg = "Error: OPENAI_API_KEY not set or provider create failed.\n";
        size_t n = 0;
        sc_wasi_fd_write(2, msg, strlen(msg), &n);
        return 1;
    }

    err = sc_wasm_channel_create(alloc, &channel);
    if (err != SC_OK) {
        provider.vtable->deinit(provider.ctx, alloc);
        return 1;
    }

    const char *model = "gpt-4o-mini";
    const char *workspace = ".";
    err = sc_agent_from_config(&agent, alloc, provider,
        NULL, 0,  /* no tools: shell not available in WASM */
        NULL, NULL, NULL, NULL,
        model, strlen(model),
        "wasm_openai", 11,
        0.7,
        workspace, 1,
        10, 30, false, 0, NULL, 0);
    if (err != SC_OK) {
        sc_wasm_channel_destroy(&channel);
        provider.vtable->deinit(provider.ctx, alloc);
        return 1;
    }

    if (channel.vtable->start(channel.ctx) != SC_OK) {
        sc_agent_deinit(&agent);
        sc_wasm_channel_destroy(&channel);
        provider.vtable->deinit(provider.ctx, alloc);
        return 1;
    }

    const char *prompt = "SeaClaw (WASI) ready. Type a message or 'exit' to quit.\n";
    size_t n = 0;
    sc_wasi_fd_write(1, prompt, strlen(prompt), &n);

    for (;;) {
        size_t line_len = 0;
        char *line = sc_wasm_channel_readline(alloc, &line_len);
        if (!line) break;

        if (line_len >= 4 && (line[0] == 'e' || line[0] == 'E') &&
            (line[1] == 'x' || line[1] == 'X') &&
            (line[2] == 'i' || line[2] == 'I') &&
            (line[3] == 't' || line[3] == 'T')) {
            alloc->free(alloc->ctx, line, line_len + 1);
            break;
        }
        if (line_len >= 4 && (line[0] == 'q' || line[0] == 'Q') &&
            (line[1] == 'u' || line[1] == 'U') &&
            (line[2] == 'i' || line[2] == 'I') &&
            (line[3] == 't' || line[3] == 'T')) {
            alloc->free(alloc->ctx, line, line_len + 1);
            break;
        }

        char *resp = NULL;
        size_t resp_len = 0;
        err = sc_agent_turn(&agent, line, line_len, &resp, &resp_len);
        alloc->free(alloc->ctx, line, line_len + 1);

        if (err != SC_OK) {
            const char *errmsg = "Error during turn.\n";
            sc_wasi_fd_write(2, errmsg, strlen(errmsg), &n);
            continue;
        }
        if (resp && resp_len > 0) {
            channel.vtable->send(channel.ctx, NULL, 0, resp, resp_len, NULL, 0);
            alloc->free(alloc->ctx, resp, resp_len);
        }
    }

    channel.vtable->stop(channel.ctx);
    sc_agent_deinit(&agent);
    sc_wasm_channel_destroy(&channel);
    provider.vtable->deinit(provider.ctx, alloc);
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    sc_allocator_t alloc = sc_wasm_allocator_default();
    if (!alloc.alloc) {
        const char *msg = "SeaClaw: allocator init failed\n";
        size_t n = 0;
        sc_wasi_fd_write(2, msg, strlen(msg), &n);
        return 1;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[1], "agent") == 0) {
            return run_agent_loop(&alloc);
        }
    }

    print_usage();
    return 0;
}

#endif /* __wasi__ */
