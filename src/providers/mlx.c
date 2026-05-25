/*
 * MLX provider — Apple Silicon frontier-model bridge.
 *
 * See include/human/providers/mlx.h for the contract.
 *
 * Build matrix:
 *   - HU_ENABLE_MLX_PROVIDER unset (default — including default CI):
 *     every vtable method returns HU_ERR_NOT_SUPPORTED. Daemon's
 *     provider fallback fires cleanly.
 *   - HU_ENABLE_MLX_PROVIDER set + __APPLE__ + __arm64__ +
 *     HU_GATEWAY_POSIX + !HU_IS_TEST: chat path invokes
 *     `mlx_run_subprocess`, which forks `python3 -m mlx_lm.generate`
 *     (mirrors the run_claude_cli pattern in claude_cli.c).
 *   - HU_ENABLE_MLX_PROVIDER set under HU_IS_TEST: helper still
 *     returns NOT_SUPPORTED so the test suite doesn't spawn Python.
 *
 * Slice 1 of M3 Phase B1 (2026-05-17) lands the subprocess scaffolding
 * inside `mlx_run_subprocess`. End-to-end test against a fixture mlx_lm
 * shim is slice 2 — see docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md.
 */

#include "human/providers/mlx.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(HU_ENABLE_MLX_PROVIDER) && defined(__APPLE__) && defined(__arm64__) && \
    defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
#define HU_MLX_SUBPROCESS_ACTIVE 1
#else
#define HU_MLX_SUBPROCESS_ACTIVE 0
#endif

#if HU_MLX_SUBPROCESS_ACTIVE
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define HU_MLX_SUBPROCESS_TIMEOUT_SECS 180
#define HU_MLX_OUTPUT_CAP              (256 * 1024)
#define HU_MLX_DEFAULT_MAX_TOKENS      512

typedef struct mlx_ctx {
    /* Owned copies so the caller's config struct can be freed after create.
     * Length is stored alongside the pointer so deinit frees the EXACT
     * allocation size (alloc->free contract requires it). Using strlen
     * at free time would break for embedded NULs or after any mutation. */
    char *model_path_owned;
    size_t model_path_owned_len;
    char *adapter_path_owned;
    size_t adapter_path_owned_len;
    int max_tokens;
} mlx_ctx_t;

static char *dup_with_len(hu_allocator_t *alloc, const char *src, size_t len) {
    if (!src || len == 0)
        return NULL;
    /* Guard the `len + 1` from overflow. SIZE_MAX would wrap to 0,
     * producing a 0-byte allocation that then memcpy's len bytes
     * (CodeRabbit 2026-05-17 finding). */
    if (len > SIZE_MAX - 1)
        return NULL;
    char *out = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!out)
        return NULL;
    memcpy(out, src, len);
    out[len] = '\0';
    return out;
}

/* ── subprocess helpers (linked build only) ──────────────────────────── */

#if HU_MLX_SUBPROCESS_ACTIVE

/* Run `python3 -m mlx_lm.generate --model <model> [--adapter-path <adapter>]
 *      --max-tokens <N> --prompt -` (prompt piped on stdin) and capture
 * stdout.
 *
 * Returns HU_OK on success with `*out` holding a NUL-terminated copy of
 * the captured output (caller frees via alloc->free with `*out_len + 1`).
 *
 * Only compiled in the linked build. The chat vtable methods short-circuit
 * to HU_ERR_NOT_SUPPORTED before calling this when HU_MLX_SUBPROCESS_ACTIVE
 * is 0 — gating the helper itself instead of putting the early-return
 * inside keeps the clang -Wunused-function gate happy in default CI. */
static hu_error_t mlx_run_subprocess(hu_allocator_t *alloc, const mlx_ctx_t *ctx,
                                     const char *prompt, size_t prompt_len, char **out,
                                     size_t *out_len) {
    if (!alloc || !ctx || !prompt || prompt_len == 0 || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ctx->model_path_owned || ctx->model_path_owned_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Build argv. Stack-allocate small buffers; mlx_lm CLI accepts
     * path arguments up to typical PATH_MAX, well within these. */
    char max_tokens_buf[16];
    int mt = (ctx->max_tokens > 0) ? ctx->max_tokens : HU_MLX_DEFAULT_MAX_TOKENS;
    snprintf(max_tokens_buf, sizeof(max_tokens_buf), "%d", mt);

    /* Worst case (with adapter):
     *   python3, -m, mlx_lm.generate,
     *   --model, <model>,
     *   --adapter-path, <adapter>,
     *   --max-tokens, <N>,
     *   --prompt, -,         (stdin sentinel)
     *   NULL
     * = 12 slots. */
    char *argv[12];
    int ai = 0;
    argv[ai++] = (char *)"python3";
    argv[ai++] = (char *)"-m";
    argv[ai++] = (char *)"mlx_lm.generate";
    argv[ai++] = (char *)"--model";
    argv[ai++] = ctx->model_path_owned;
    if (ctx->adapter_path_owned && ctx->adapter_path_owned_len > 0) {
        argv[ai++] = (char *)"--adapter-path";
        argv[ai++] = ctx->adapter_path_owned;
    }
    argv[ai++] = (char *)"--max-tokens";
    argv[ai++] = max_tokens_buf;
    argv[ai++] = (char *)"--prompt";
    argv[ai++] = (char *)"-"; /* read prompt from stdin */
    argv[ai] = NULL;

    int stdout_fds[2];
    int stdin_fds[2];
    if (pipe(stdout_fds) != 0)
        return HU_ERR_IO;
    if (pipe(stdin_fds) != 0) {
        close(stdout_fds[0]);
        close(stdout_fds[1]);
        return HU_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_fds[0]);
        close(stdout_fds[1]);
        close(stdin_fds[0]);
        close(stdin_fds[1]);
        return HU_ERR_IO;
    }

    if (pid == 0) {
        /* Child: wire stdin/stdout to pipes and exec. */
        close(stdout_fds[0]);
        close(stdin_fds[1]);
        dup2(stdin_fds[0], STDIN_FILENO);
        dup2(stdout_fds[1], STDOUT_FILENO);
        dup2(stdout_fds[1], STDERR_FILENO);
        close(stdin_fds[0]);
        close(stdout_fds[1]);
        execvp("python3", argv);
        _exit(127);
    }

    close(stdout_fds[1]);
    close(stdin_fds[0]);

    /* Write prompt to child stdin, then close to signal EOF. */
    size_t written = 0;
    while (written < prompt_len) {
        ssize_t w = write(stdin_fds[1], prompt + written, prompt_len - written);
        if (w <= 0)
            break;
        written += (size_t)w;
    }
    close(stdin_fds[1]);

    if (written < prompt_len) {
        close(stdout_fds[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return HU_ERR_IO;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, HU_MLX_OUTPUT_CAP);
    if (!buf) {
        close(stdout_fds[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    bool timed_out = false;
    for (;;) {
        if (len >= HU_MLX_OUTPUT_CAP - 1)
            break;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(stdout_fds[0], &rfds);
        struct timeval tv = {.tv_sec = HU_MLX_SUBPROCESS_TIMEOUT_SECS, .tv_usec = 0};
        int sel = select(stdout_fds[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            timed_out = true;
            break;
        }
        ssize_t n = read(stdout_fds[0], buf + len, HU_MLX_OUTPUT_CAP - len - 1);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(stdout_fds[0]);

    if (timed_out) {
        alloc->free(alloc->ctx, buf, HU_MLX_OUTPUT_CAP);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return HU_ERR_TIMEOUT;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (len == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        alloc->free(alloc->ctx, buf, HU_MLX_OUTPUT_CAP);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    /* Trim trailing whitespace mlx_lm.generate emits a trailing
     * newline; the caller's content field shouldn't carry it. */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        len--;
    buf[len] = '\0';

    char *result = hu_strndup(alloc, buf, len);
    alloc->free(alloc->ctx, buf, HU_MLX_OUTPUT_CAP);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    *out = result;
    *out_len = len;
    return HU_OK;
}

/* Flatten a chat request's system + user messages into a single prompt.
 * Modeled after the claude_cli pattern. Returns the combined buffer
 * length; caller-owned `combined` is filled. Returns 0 on overflow. */
static size_t flatten_chat_request(const hu_chat_request_t *request, char *combined, size_t cap) {
    const char *sys = NULL;
    size_t sys_len = 0;
    const char *user = NULL;
    size_t user_len = 0;
    for (size_t i = 0; i < request->messages_count; i++) {
        if (request->messages[i].role == HU_ROLE_SYSTEM && request->messages[i].content_len > 0) {
            sys = request->messages[i].content;
            sys_len = request->messages[i].content_len;
        }
        if (request->messages[i].role == HU_ROLE_USER && request->messages[i].content_len > 0) {
            user = request->messages[i].content;
            user_len = request->messages[i].content_len;
        }
    }
    if (!user)
        return 0;
    if (sys && sys_len > 0) {
        if (sys_len + user_len + 4 >= cap)
            return 0;
        memcpy(combined, sys, sys_len);
        combined[sys_len] = '\n';
        combined[sys_len + 1] = '\n';
        memcpy(combined + sys_len + 2, user, user_len);
        return sys_len + 2 + user_len;
    }
    if (user_len + 1 >= cap)
        return 0;
    memcpy(combined, user, user_len);
    return user_len;
}

#endif /* HU_MLX_SUBPROCESS_ACTIVE */

/* ── vtable: chat ─────────────────────────────────────────────────────── */

static hu_error_t mlx_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                           const char *model, size_t model_len, double temperature,
                           hu_chat_response_t *out) {
    (void)model;
    (void)model_len;
    (void)temperature;
    if (!ctx || !alloc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;

#if !HU_MLX_SUBPROCESS_ACTIVE
    /* Unlinked / test build: do not touch `out`. The daemon's fallback
     * path relies on out being untouched on NOT_SUPPORTED so a retry
     * with another provider sees a clean slate. */
    return HU_ERR_NOT_SUPPORTED;
#else
    mlx_ctx_t *c = (mlx_ctx_t *)ctx;
    char combined[65536];
    size_t combined_len = flatten_chat_request(request, combined, sizeof(combined));
    if (combined_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char *text = NULL;
    size_t text_len = 0;
    hu_error_t err = mlx_run_subprocess(alloc, c, combined, combined_len, &text, &text_len);
    if (err != HU_OK)
        return err;

    memset(out, 0, sizeof(*out));
    out->content = text;
    out->content_len = text_len;
    /* Echo back the configured model path so the response carries
     * provenance — matches the claude_cli convention. */
    if (c->model_path_owned && c->model_path_owned_len > 0) {
        out->model = hu_strndup(alloc, c->model_path_owned, c->model_path_owned_len);
        out->model_len = c->model_path_owned_len;
    }
    return HU_OK;
#endif
}

static hu_error_t mlx_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *system_prompt,
                                       size_t system_len, const char *message, size_t message_len,
                                       const char *model, size_t model_len, double temperature,
                                       char **out, size_t *out_len) {
    (void)model;
    (void)model_len;
    (void)temperature;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;

#if !HU_MLX_SUBPROCESS_ACTIVE
    (void)ctx;
    (void)alloc;
    (void)system_prompt;
    (void)system_len;
    (void)message;
    (void)message_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!ctx || !alloc || !message || message_len == 0 || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    mlx_ctx_t *c = (mlx_ctx_t *)ctx;
    char combined[65536];
    size_t combined_len;
    if (system_prompt && system_len > 0) {
        if (system_len + message_len + 4 >= sizeof(combined))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(combined, system_prompt, system_len);
        combined[system_len] = '\n';
        combined[system_len + 1] = '\n';
        memcpy(combined + system_len + 2, message, message_len);
        combined_len = system_len + 2 + message_len;
    } else {
        if (message_len + 1 >= sizeof(combined))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(combined, message, message_len);
        combined_len = message_len;
    }
    return mlx_run_subprocess(alloc, c, combined, combined_len, out, out_len);
#endif
}

static bool mlx_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static const char *mlx_get_name(void *ctx) {
    (void)ctx;
    return "mlx";
}

static void mlx_deinit(void *ctx, hu_allocator_t *alloc) {
    mlx_ctx_t *c = (mlx_ctx_t *)ctx;
    if (!c)
        return;
    if (c->model_path_owned)
        alloc->free(alloc->ctx, c->model_path_owned, c->model_path_owned_len + 1);
    if (c->adapter_path_owned)
        alloc->free(alloc->ctx, c->adapter_path_owned, c->adapter_path_owned_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

/* ── vtable: load_adapter ─────────────────────────────────────────────── */

/* Phase B5 (2026-05-19): wire adapter delivery via the subprocess argv.
 *
 * Contract:
 *   - Validate inputs (ctx, alloc, adapter_path).
 *   - Verify the directory contains `adapters.safetensors` (mlx-lm CLI
 *     loads weights from this filename when given --adapter-path <dir>).
 *   - Persist the path on the ctx so subsequent `mlx_run_subprocess`
 *     invocations include `--adapter-path <path>` in argv.
 *   - On failure, log a warning and return a precise error code.
 *
 * Delivery mechanism:
 *   - Subprocess path: mlx_run_subprocess already passes
 *     `--adapter-path <ctx->adapter_path_owned>` when the field is
 *     non-NULL (see argv builder above). After this call the next
 *     chat will use the new adapter.
 *   - HTTP admin path (server-mode MLX): the daemon's per-turn router
 *     `hu_agent_m3_route_per_turn` POSTs to /v1/adapters/swap directly
 *     via `hu_mlx_admin_swap_adapter` (see src/agent/agent.c:1053). The
 *     provider vtable layer remains subprocess-only; the admin path is
 *     a parallel mechanism the agent invokes by URL.
 *
 * NOTE on dispatcher safety: when HU_MLX_SUBPROCESS_ACTIVE is 0 (the
 * default CI / test build), the chat path returns NOT_SUPPORTED anyway,
 * so persisting an adapter on ctx has no observable effect there. The
 * load itself still validates and persists — this keeps the load path
 * uniformly testable and avoids a confusing "succeeds-on-mac,
 * fails-on-linux" surface for callers.
 */
static hu_error_t mlx_load_adapter(void *ctx, hu_allocator_t *alloc, const char *adapter_path,
                                   size_t adapter_path_len, const char *adapter_id,
                                   size_t adapter_id_len) {
    (void)adapter_id;
    (void)adapter_id_len;

    if (!ctx || !alloc || !adapter_path || adapter_path_len == 0) {
        hu_log_warn("mlx_provider", NULL,
                    "load_adapter: NULL or empty argument (ctx=%p alloc=%p path=%p len=%zu)", ctx,
                    (void *)alloc, (const void *)adapter_path, adapter_path_len);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Validate the adapter directory contains adapters.safetensors.
     * Use access(F_OK) — minimal syscall, just an existence check.
     * The mlx-lm CLI will re-validate when it actually loads the file. */
    char check[1024];
    int n = snprintf(check, sizeof(check), "%.*s/adapters.safetensors", (int)adapter_path_len,
                     adapter_path);
    if (n <= 0 || (size_t)n >= sizeof(check)) {
        hu_log_warn("mlx_provider", NULL, "load_adapter: adapter path too long (len=%zu cap=%zu)",
                    adapter_path_len, sizeof(check));
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (access(check, F_OK) != 0) {
        hu_log_warn("mlx_provider", NULL, "load_adapter: adapters.safetensors not found in %.*s",
                    (int)adapter_path_len, adapter_path);
        return HU_ERR_NOT_FOUND;
    }

    /* US-6: Validate safetensors magic bytes (first 8 bytes) to reject corrupted
     * adapters early. safetensors format: 8-byte LE length prefix followed by
     * JSON header. Reject files that don't have enough data or have invalid magic. */
    FILE *f = fopen(check, "rb");
    if (!f) {
        hu_log_warn("mlx_provider", NULL, "load_adapter: failed to open %s for validation", check);
        return HU_ERR_INVALID_ARGUMENT;
    }

    unsigned char magic[8] = {0};
    size_t read_count = fread(magic, 1, 8, f);
    fclose(f);

    if (read_count < 8) {
        hu_log_warn("mlx_provider", NULL,
                    "load_adapter: safetensors file too small (got %zu bytes, need 8 for header)",
                    read_count);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Safetensors format starts with a 64-bit LE integer indicating header size.
     * Valid files have a reasonable header size (typically 100-10000 bytes).
     * Arbitrary magic bytes like "BADBADBA" would have mismatched size fields.
     * For a quick sanity check, verify the first 8 bytes form a plausible
     * header size. Valid sizes are in range [0, 1MB) — allow 0 for test
     * fixtures and empty/truncated files, but reject impossibly large sizes. */
    uint64_t header_size = (uint64_t)magic[0] | ((uint64_t)magic[1] << 8) |
                           ((uint64_t)magic[2] << 16) | ((uint64_t)magic[3] << 24) |
                           ((uint64_t)magic[4] << 32) | ((uint64_t)magic[5] << 40) |
                           ((uint64_t)magic[6] << 48) | ((uint64_t)magic[7] << 56);

    /* Reasonable header sizes: 0 (fixture marker) to 1MB (max plausible JSON) */
    if (header_size > (1024 * 1024)) {
        hu_log_warn("mlx_provider", NULL,
                    "load_adapter: safetensors header size implausible (%llu bytes); "
                    "file may be corrupted",
                    (unsigned long long)header_size);
        return HU_ERR_INVALID_ARGUMENT;
    }

    mlx_ctx_t *c = (mlx_ctx_t *)ctx;
    char *new_path = dup_with_len(alloc, adapter_path, adapter_path_len);
    if (!new_path)
        return HU_ERR_OUT_OF_MEMORY;

    /* Swap atomically from the caller's POV: free the prior path AFTER
     * the new one is fully allocated, so a failed alloc leaves the
     * previously-active adapter intact (consistent with personal-model
     * save's tmp+rename pattern). */
    if (c->adapter_path_owned) {
        alloc->free(alloc->ctx, c->adapter_path_owned, c->adapter_path_owned_len + 1);
    }
    c->adapter_path_owned = new_path;
    c->adapter_path_owned_len = adapter_path_len;
    return HU_OK;
}

/* Test-mode introspection: returns the persisted adapter path, or NULL
 * if none. Used by tests/test_mlx_load_adapter.c to verify the
 * subprocess argv builder would receive the new value without spawning
 * python3. Defined in the linked build too — the read is O(1). */
const char *hu_mlx_provider_active_adapter_path(const hu_provider_t *p, size_t *out_len) {
    if (!p || !p->ctx)
        return NULL;
    const mlx_ctx_t *c = (const mlx_ctx_t *)p->ctx;
    if (out_len)
        *out_len = c->adapter_path_owned_len;
    return c->adapter_path_owned;
}

static hu_error_t mlx_unload_adapter(void *ctx, const char *adapter_id, size_t adapter_id_len) {
    (void)ctx;
    (void)adapter_id;
    (void)adapter_id_len;
    /* Bugbot 2026-05-16: vtable previously set load_adapter + active_adapter
     * explicitly but left unload_adapter as NULL. The hu_provider_unload_adapter
     * dispatcher tolerates NULL safely, but any direct vtable->unload_adapter(...)
     * call (some CLI adapter-management paths do this) would deref NULL and
     * crash. Set all three of the adapter triple — even when the body is a
     * NOT_SUPPORTED stub — to keep the vtable consistent. */
    return HU_ERR_NOT_SUPPORTED;
}

static const char *mlx_active_adapter(void *ctx) {
    (void)ctx;
    return NULL;
}

/* ── vtable ───────────────────────────────────────────────────────────── */

static const hu_provider_vtable_t mlx_vtable = {
    .chat = mlx_chat,
    .chat_with_system = mlx_chat_with_system,
    .supports_native_tools = mlx_supports_native_tools,
    .get_name = mlx_get_name,
    .deinit = mlx_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .stream_chat = NULL,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .load_adapter = mlx_load_adapter,
    .unload_adapter = mlx_unload_adapter,
    .active_adapter = mlx_active_adapter,
};

hu_error_t hu_mlx_provider_create(hu_allocator_t *alloc, const hu_mlx_config_t *config,
                                  hu_provider_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    mlx_ctx_t *c = (mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));

    if (config) {
        c->model_path_owned = dup_with_len(alloc, config->model_path, config->model_path_len);
        c->model_path_owned_len = c->model_path_owned ? config->model_path_len : 0;
        c->adapter_path_owned = dup_with_len(alloc, config->adapter_path, config->adapter_path_len);
        c->adapter_path_owned_len = c->adapter_path_owned ? config->adapter_path_len : 0;
        c->max_tokens = config->max_tokens;
        /* If the caller asked for a model/adapter but allocation failed,
         * surface OOM rather than silently dropping the request. */
        if ((config->model_path && config->model_path_len > 0 && !c->model_path_owned) ||
            (config->adapter_path && config->adapter_path_len > 0 && !c->adapter_path_owned)) {
            mlx_deinit(c, alloc);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    out->ctx = c;
    out->vtable = &mlx_vtable;
    return HU_OK;
}
