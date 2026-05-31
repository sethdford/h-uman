/* Human WASI entrypoint — full agent loop for WASM target.
 * Limitations: no shell tool, no process spawning, config from WASI fs or defaults. */
#ifdef __wasi__

#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/wasm/wasi_bindings.h"
#include "human/wasm/wasm_alloc.h"
#include "human/wasm/wasm_channel.h"
#include "human/wasm/wasm_provider.h"
#include <stdio.h>
#include <string.h>

#define HU_VERSION           "0.5.0"
#define HU_CODENAME          "Human"
#define HU_WASI_PREOPEN_ROOT 3

static void print_usage(void) {
    const char *msg = "Usage: human agent [options]\n"
                      "  agent  Start interactive CLI (stdin/stdout)\n"
                      "  -h     Show help\n";
    size_t n = 0;
    hu_wasi_fd_write(1, msg, strlen(msg), &n);
}

static void print_version(void) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s v%s (WASI) — not quite human.\n", HU_CODENAME, HU_VERSION);
    if (len > 0) {
        size_t n = 0;
        hu_wasi_fd_write(1, buf, (size_t)len, &n);
    }
}

static int run_agent_loop(hu_allocator_t *alloc) {
    hu_provider_t provider;
    hu_channel_t channel;
    hu_agent_t agent;

    const char *api_key = getenv("OPENAI_API_KEY");
    size_t api_key_len = api_key ? strlen(api_key) : 0;
    hu_error_t err = hu_wasm_provider_create(alloc, api_key, api_key_len, NULL, 0, &provider);
    if (err != HU_OK) {
        const char *msg = "Error: OPENAI_API_KEY not set or provider create failed.\n";
        size_t n = 0;
        hu_wasi_fd_write(2, msg, strlen(msg), &n);
        return 1;
    }

    err = hu_wasm_channel_create(alloc, &channel);
    if (err != HU_OK) {
        provider.vtable->deinit(provider.ctx, alloc);
        return 1;
    }

    const char *model = "gpt-4o-mini";
    const char *workspace = ".";
    err = hu_agent_from_config(&agent, alloc, provider, NULL,
                               0, /* no tools: shell not available in WASM */
                               NULL, NULL, NULL, NULL, model, strlen(model), "wasm_openai", 11, 0.7,
                               workspace, 1, 10, 30, false, 0, NULL, 0, NULL, 0, NULL);
    if (err != HU_OK) {
        hu_wasm_channel_destroy(&channel);
        provider.vtable->deinit(provider.ctx, alloc);
        return 1;
    }

    if (channel.vtable->start(channel.ctx) != HU_OK) {
        hu_agent_deinit(&agent);
        hu_wasm_channel_destroy(&channel);
        provider.vtable->deinit(provider.ctx, alloc);
        return 1;
    }

    const char *prompt = "Human (WASI) ready. Type a message or 'exit' to quit.\n";
    size_t n = 0;
    hu_wasi_fd_write(1, prompt, strlen(prompt), &n);

    for (;;) {
        size_t line_len = 0;
        char *line = hu_wasm_channel_readline(alloc, &line_len);
        if (!line)
            break;

        if (line_len >= 4 && (line[0] == 'e' || line[0] == 'E') &&
            (line[1] == 'x' || line[1] == 'X') && (line[2] == 'i' || line[2] == 'I') &&
            (line[3] == 't' || line[3] == 'T')) {
            alloc->free(alloc->ctx, line, line_len + 1);
            break;
        }
        if (line_len >= 4 && (line[0] == 'q' || line[0] == 'Q') &&
            (line[1] == 'u' || line[1] == 'U') && (line[2] == 'i' || line[2] == 'I') &&
            (line[3] == 't' || line[3] == 'T')) {
            alloc->free(alloc->ctx, line, line_len + 1);
            break;
        }

        char *resp = NULL;
        size_t resp_len = 0;
        err = hu_agent_turn(&agent, line, line_len, &resp, &resp_len);
        alloc->free(alloc->ctx, line, line_len + 1);

        if (err != HU_OK) {
            const char *errmsg = "Error during turn.\n";
            hu_wasi_fd_write(2, errmsg, strlen(errmsg), &n);
            continue;
        }
        if (resp && resp_len > 0) {
            channel.vtable->send(channel.ctx, NULL, 0, resp, resp_len, NULL, 0);
            alloc->free(alloc->ctx, resp, resp_len + 1);
        }
    }

    channel.vtable->stop(channel.ctx);
    hu_agent_deinit(&agent);
    hu_wasm_channel_destroy(&channel);
    provider.vtable->deinit(provider.ctx, alloc);
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    hu_allocator_t alloc = hu_wasm_allocator_default();
    if (!alloc.alloc) {
        const char *msg = "Human: allocator init failed\n";
        size_t n = 0;
        hu_wasi_fd_write(2, msg, strlen(msg), &n);
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
