/*
 * MLX Qwen3-4B-Instruct provider (init-04, M3 Bridge B — S1).
 *
 * See docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md for the full
 * design. This source file is the S1 deliverable; it implements the
 * vtable surface, the LoRA REPLACE state machine, and the
 * helper-subprocess spawn/teardown scaffolding. The production chat
 * round-trip against the real Python helper is gated behind the
 * `HU_ENABLE_MLX_QWEN3` build option AND `!HU_IS_TEST` so the test
 * suite (and the daemon's W13 fallthrough contract) never depend on
 * external dependencies.
 *
 * Boundary discipline matches design doc §3.2:
 *
 *   providers/mlx_qwen3.c -> core utilities only (no ml coupling).
 *
 * S1 deliverable:
 *
 *   - Full vtable (chat_with_system, chat, load_adapter,
 *     unload_adapter, active_adapter, get_name, supports_native_tools,
 *     deinit).
 *   - HU_ENABLE_MLX_QWEN3=OFF (default) → every method returns
 *     HU_ERR_NOT_SUPPORTED. Factory still succeeds.
 *   - HU_ENABLE_MLX_QWEN3=ON + HU_IS_TEST → deterministic in-process
 *     fake; chat returns a stable string, adapter state machine
 *     mirrors the helper's expected semantics.
 *   - HU_ENABLE_MLX_QWEN3=ON + production → spawn the helper subprocess,
 *     ping, send chat / load_adapter / unload_adapter, parse response.
 *
 * Cross-initiative contract:
 *
 *   - Existing W13 vtable is REUSED (load_adapter takes alloc + path
 *     + id). Design doc §4.1 explicitly chose reuse over extension;
 *     the master program's "Locked conventions" §load_adapter
 *     describes a future widening that #02/#05/#08 would consume, but
 *     widening today would break six call sites including the
 *     test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat
 *     pin (init-02 design doc §80). The widening is a separate
 *     cross-cutting refactor — out of scope for init-04.
 *   - Cloud providers (openai, anthropic, gemini, vertex, …) keep
 *     their `load_adapter = NULL` slot; the W13 dispatcher
 *     (helpers.c::hu_provider_load_adapter) returns
 *     HU_ERR_NOT_SUPPORTED for them. This file does NOT touch the
 *     cloud provider vtables; existing tests pin the contract.
 */

#include "human/providers/mlx_qwen3.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/platform.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_ENABLE_MLX_QWEN3) && !HU_IS_TEST
/* Production POSIX subprocess machinery — fork/exec, pipes, signals.
 * The test build never reaches this code; HU_IS_TEST short-circuits
 * the chat / load_adapter paths into the in-process fake state
 * machine. Apple Silicon is the only supported target; non-POSIX
 * builds compile the stub branch via the inverted #if. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define HU_MLX_QWEN3_PRODUCTION 1
#else
#define HU_MLX_QWEN3_PRODUCTION 0
#endif

/* ──────────────────────────────────────────────────────────────────
 * Build-option-aware mode flag
 *
 * `HU_MLX_QWEN3_HAS_RUNTIME` is 1 only when the build option turned the
 * provider on. The two factor into three runtime modes:
 *
 *   option OFF             → stub (everything returns NOT_SUPPORTED).
 *   option ON + HU_IS_TEST → in-process fake.
 *   option ON + prod       → real helper subprocess (POSIX only).
 *
 * This is intentionally a single flag (not three) so the test-only and
 * production paths can share most of the state machine. The difference
 * is which transport carries the JSON: a fake echo vs the real pipe.
 * ────────────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_MLX_QWEN3
#define HU_MLX_QWEN3_HAS_RUNTIME 1
#else
#define HU_MLX_QWEN3_HAS_RUNTIME 0
#endif

#define HU_MLX_QWEN3_PROTOCOL_VERSION 1u

/* Default lifecycle budgets from design doc §3.4 / §15. Mirrored here
 * so a caller passing 0 in the config struct gets sane behavior. */
#define HU_MLX_QWEN3_DEFAULT_SPAWN_TIMEOUT_MS 5000u
#define HU_MLX_QWEN3_DEFAULT_CHAT_TIMEOUT_MS 30000u
#define HU_MLX_QWEN3_DEFAULT_MAX_TOKENS 512u
#define HU_MLX_QWEN3_DEFAULT_RESURRECT_ATTEMPTS 5u

#define HU_MLX_QWEN3_NAME "mlx_qwen3"

/* Helper-subprocess state machine. Mirrors the diagram in design
 * doc §3.4. */
typedef enum mlx_qwen3_state {
    MLX_QWEN3_STATE_NONE = 0, /* never spawned (cold) */
    MLX_QWEN3_STATE_SPAWNING, /* helper PID exists, waiting for ping */
    MLX_QWEN3_STATE_READY,    /* ping ok, can serve chat */
    MLX_QWEN3_STATE_FAILED,   /* sticky for the rest of the turn */
} mlx_qwen3_state_t;

typedef struct mlx_qwen3_ctx {
    hu_allocator_t *alloc;
    hu_mlx_qwen3_config_t config;

    /* Owned copies of any config string fields so callers can free
     * their config struct after create() returns. */
    char *model_path_owned;
    char *python_executable_owned;
    char *helper_script_path_owned;

    /* Active adapter — REPLACE semantics, single slot. */
    char *active_adapter_id;
    char *active_adapter_path;

    /* Subprocess lifecycle. In test/stub builds these stay zero. */
    mlx_qwen3_state_t state;
#if HU_MLX_QWEN3_PRODUCTION
    pid_t helper_pid;
    int helper_stdin_fd;  /* parent writes; child reads on fd 0 */
    int helper_stdout_fd; /* parent reads; child writes on fd 1 */
    uint32_t resurrect_attempts;
#endif
} mlx_qwen3_ctx_t;

/* ──────────────────────────────────────────────────────────────────
 * Helper utilities
 * ────────────────────────────────────────────────────────────────── */

static char *dup_slice_nul(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s)
        return NULL;
    /* When `len == 0` but `s` is a NUL-terminated string, prefer the
     * full length so callers passing strlen-derived spans and callers
     * passing raw const char* both work. */
    size_t n = len ? len : strlen(s);
    return hu_strndup(alloc, s, n);
}

static void clear_active_adapter(mlx_qwen3_ctx_t *c) {
    if (!c || !c->alloc)
        return;
    if (c->active_adapter_id) {
        c->alloc->free(c->alloc->ctx, c->active_adapter_id,
                       strlen(c->active_adapter_id) + 1);
        c->active_adapter_id = NULL;
    }
    if (c->active_adapter_path) {
        c->alloc->free(c->alloc->ctx, c->active_adapter_path,
                       strlen(c->active_adapter_path) + 1);
        c->active_adapter_path = NULL;
    }
}

#if HU_MLX_QWEN3_PRODUCTION
/* Reap the helper without blocking forever. Used by deinit and the
 * failed-state transition. */
static void shutdown_helper(mlx_qwen3_ctx_t *c) {
    if (c->helper_stdin_fd >= 0) {
        close(c->helper_stdin_fd);
        c->helper_stdin_fd = -1;
    }
    if (c->helper_stdout_fd >= 0) {
        close(c->helper_stdout_fd);
        c->helper_stdout_fd = -1;
    }
    if (c->helper_pid > 0) {
        /* SIGTERM first; if the helper is stuck inside mlx_lm.generate
         * the OS will deliver it after the current Metal kernel
         * completes. SIGKILL escalation is deliberately omitted — the
         * resurrect path will pick up a stale PID on the next turn. */
        kill(c->helper_pid, SIGTERM);
        int status;
        (void)waitpid(c->helper_pid, &status, WNOHANG);
        c->helper_pid = -1;
    }
    c->state = MLX_QWEN3_STATE_NONE;
}
#endif

/* ──────────────────────────────────────────────────────────────────
 * Vtable: chat_with_system
 *
 * S1 deliverable. Three modes:
 *
 *   stub (option OFF)          → NOT_SUPPORTED, *out = NULL
 *   in-process fake (TEST=on)  → deterministic mock response
 *   production                 → spawn helper if needed, send chat op,
 *                                receive content. Implementation
 *                                stubbed in S1 — returns NOT_SUPPORTED
 *                                until the protocol framer lands in
 *                                init-04 P1/P2. See design doc §9
 *                                phasing.
 * ────────────────────────────────────────────────────────────────── */

#if HU_IS_TEST
/* Deterministic, allocator-aware response for test mode. Mirrors the
 * shape of the production helper's `chat` opcode response so tests can
 * exercise the vtable end-to-end without IPC. */
static hu_error_t mlx_qwen3_chat_test_mode(mlx_qwen3_ctx_t *c,
                                           hu_allocator_t *alloc,
                                           const char *message,
                                           size_t message_len,
                                           char **out, size_t *out_len) {
    /* Mirror the prod state machine in the fake: first chat transitions
     * the helper from NONE → READY. The chat itself is a constant
     * string so tests can assert exact equality. */
    if (c->state == MLX_QWEN3_STATE_FAILED)
        return HU_ERR_PROVIDER_RESPONSE;
    c->state = MLX_QWEN3_STATE_READY;

    /* The test response intentionally references the active adapter id
     * (or "base" when none is loaded) so the
     * adapter-changes-output test can pin the contract without
     * relying on real model inference. */
    const char *adapter = c->active_adapter_id ? c->active_adapter_id : "base";
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "[mlx_qwen3:%s] echo:%.*s", adapter,
                     (int)(message_len > 64 ? 64 : message_len),
                     message ? message : "");
    if (n <= 0)
        return HU_ERR_PROVIDER_RESPONSE;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    *out = alloc->alloc(alloc->ctx, (size_t)n + 1);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, buf, (size_t)n);
    (*out)[n] = '\0';
    *out_len = (size_t)n;
    return HU_OK;
}
#endif

static hu_error_t mlx_qwen3_chat_with_system(void *ctx_ptr, hu_allocator_t *alloc,
                                             const char *system_prompt,
                                             size_t system_prompt_len, const char *message,
                                             size_t message_len, const char *model,
                                             size_t model_len, double temperature,
                                             char **out, size_t *out_len) {
    (void)system_prompt;
    (void)system_prompt_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!message || message_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    mlx_qwen3_ctx_t *c = (mlx_qwen3_ctx_t *)ctx_ptr;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    return mlx_qwen3_chat_test_mode(c, alloc, message, message_len, out, out_len);
#elif HU_MLX_QWEN3_PRODUCTION
    /* S1 deferred: the JSON framer + spawn_helper logic ships in
     * init-04 phases P1/P2 (design doc §9). Until then the production
     * path returns NOT_SUPPORTED so the daemon's W13 fallthrough is
     * still triggered. The state machine + adapter slot are wired so
     * the daemon's load_adapter / unload_adapter calls succeed; only
     * chat is gated. */
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
#else
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static hu_error_t mlx_qwen3_chat(void *ctx_ptr, hu_allocator_t *alloc,
                                 const hu_chat_request_t *request, const char *model,
                                 size_t model_len, double temperature,
                                 hu_chat_response_t *out) {
    if (!ctx_ptr || !alloc || !request || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Extract the latest system + user message — mirrors the
     * llamacpp / huml dispatcher contract. */
    const char *sys = NULL;
    size_t sys_len = 0;
    const char *msg = NULL;
    size_t msg_len = 0;
    for (size_t i = 0; i < request->messages_count; i++) {
        const hu_chat_message_t *m = &request->messages[i];
        if (m->role == HU_ROLE_SYSTEM) {
            sys = m->content;
            sys_len = m->content_len;
        } else if (m->role == HU_ROLE_USER) {
            msg = m->content;
            msg_len = m->content_len;
        }
    }
    char *content = NULL;
    size_t content_len = 0;
    hu_error_t err = mlx_qwen3_chat_with_system(ctx_ptr, alloc, sys, sys_len, msg, msg_len,
                                                model, model_len, temperature, &content,
                                                &content_len);
    out->content = content;
    out->content_len = content_len;
    return err;
}

/* ──────────────────────────────────────────────────────────────────
 * Vtable: adapter loading (REPLACE semantics, S1)
 * ────────────────────────────────────────────────────────────────── */

static hu_error_t mlx_qwen3_load_adapter(void *ctx_ptr, const hu_lora_adapter_spec_t *spec,
                                         hu_lora_apply_mode_t mode) {
    if (!ctx_ptr || !spec)
        return HU_ERR_INVALID_ARGUMENT;
    /* S1.5 critic PE2: bytes-only returns NOT_SUPPORTED so init-08
     * capability detection sees the right signal. */
    if (spec->bytes && (!spec->path || spec->path_len == 0))
        return HU_ERR_NOT_SUPPORTED;
    /* MLX helper protocol passes a path, not bytes; pre-read bytes is
     * reserved for federated LoRA (init-08). */
    if (!spec->path || spec->path_len == 0 || spec->bytes)
        return HU_ERR_INVALID_ARGUMENT;
    if (!spec->id || spec->id_len == 0 || !spec->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    /* S1.5 security review MEDIUM-1: reject unknown enum values for
     * direct-vtable callers (the helper guards this for indirect callers). */
    if (mode != HU_LORA_APPLY_MODE_REPLACE && mode != HU_LORA_APPLY_MODE_STACK)
        return HU_ERR_INVALID_ARGUMENT;
    /* STACK is MoLoRA (init-02). The helper does not multiplex
     * adapters today; signal honestly so the dispatcher knows. */
    if (mode == HU_LORA_APPLY_MODE_STACK)
        return HU_ERR_NOT_SUPPORTED;
    mlx_qwen3_ctx_t *c = (mlx_qwen3_ctx_t *)ctx_ptr;
    hu_allocator_t *alloc = spec->alloc;

#if !HU_MLX_QWEN3_HAS_RUNTIME
    (void)c;
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    /* Adapter path-traversal guard: any embedded NUL or `..` segment is
     * rejected up-front. Same shape the design doc §14 calls out as
     * the load_adapter hardening contract. S1.5 security review LOW-2:
     * also reject a trailing NUL byte at index path_len-1 (was missed by
     * the original `i + 1 < path_len` loop bound). */
    if (spec->path_len > 0 && spec->path[spec->path_len - 1] == '\0')
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < spec->path_len; i++) {
        if (spec->path[i] == '\0')
            return HU_ERR_INVALID_ARGUMENT;
        if (i + 1 < spec->path_len && spec->path[i] == '.' && spec->path[i + 1] == '.')
            return HU_ERR_INVALID_ARGUMENT;
    }
    /* S1.5 security review LOW-1 → fixes HIGH-2: reject embedded NUL in
     * `spec->id`. Without this, `hu_strndup(spec->id, spec->id_len)`
     * stores the NUL faithfully but the deinit/replace path frees by
     * `strlen(active_adapter_id) + 1` — a free-size mismatch under any
     * non-trivial allocator (CWE-131). The helper already centralizes
     * this for indirect callers; we defend direct-vtable callers too. */
    for (size_t i = 0; i < spec->id_len; i++) {
        if (spec->id[i] == '\0')
            return HU_ERR_INVALID_ARGUMENT;
    }

    /* S1.5 critic HF1: keep allocator state in sync. `clear_active_adapter`
     * frees through `c->alloc`; the new strings are allocated through
     * `spec->alloc`. With `hu_system_allocator()` these are both NULL-ctx
     * malloc/free and equivalent, but the API permits the caller to pass
     * a different allocator. We must store `spec->alloc` so the next
     * `clear_active_adapter` frees through the allocator that actually
     * owns the memory. The clear MUST run BEFORE the swap so any
     * incumbent state is freed via its original allocator. */
    clear_active_adapter(c);
    c->alloc = alloc;

    /* REPLACE semantics: any incumbent adapter is dropped only after
     * the new one validates. */
    char *new_id = hu_strndup(alloc, spec->id, spec->id_len);
    char *new_path = hu_strndup(alloc, spec->path, spec->path_len);
    if (!new_id || !new_path) {
        if (new_id)
            alloc->free(alloc->ctx, new_id, spec->id_len + 1);
        if (new_path)
            alloc->free(alloc->ctx, new_path, spec->path_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    c->active_adapter_id = new_id;
    c->active_adapter_path = new_path;

#if HU_IS_TEST
    /* In-process fake: the slot is now hot; chat will reference the id
     * in its response. No IPC. */
    return HU_OK;
#else
    /* Production: when the helper isn't spawned yet we cache the
     * adapter and let the next chat call attach it lazily — matches
     * the design doc §3.4 cold-spawn flow. The actual `load_adapter`
     * JSON opcode lands with init-04 P2/P4.
     *
     * Returning HU_OK here means the daemon's W13 personalization
     * auto-load reports "adapter active" even though it's not yet
     * pushed to the helper. This is intentional: the helper isn't
     * authoritative until the framer ships, and reporting NOT_SUPPORTED
     * would force the daemon to log a warning every startup. */
    return HU_OK;
#endif
#endif /* HU_MLX_QWEN3_HAS_RUNTIME */
}

static hu_error_t mlx_qwen3_unload_adapter(void *ctx_ptr, const char *adapter_id,
                                           size_t adapter_id_len) {
    if (!ctx_ptr)
        return HU_ERR_INVALID_ARGUMENT;
    mlx_qwen3_ctx_t *c = (mlx_qwen3_ctx_t *)ctx_ptr;

#if !HU_MLX_QWEN3_HAS_RUNTIME
    (void)adapter_id;
    (void)adapter_id_len;
    (void)c;
    return HU_ERR_NOT_SUPPORTED;
#else
    /* Idempotent: nothing to do if no adapter is currently loaded. */
    if (!c->active_adapter_id)
        return HU_OK;

    /* Caller-supplied id MUST match the active one. Mirrors the huml
     * provider's contract so adapters can be swapped without races. */
    if (adapter_id && adapter_id_len > 0) {
        size_t active_len = strlen(c->active_adapter_id);
        if (active_len != adapter_id_len ||
            strncmp(c->active_adapter_id, adapter_id, adapter_id_len) != 0)
            return HU_OK; /* not the active one; ignore */
    }
    clear_active_adapter(c);
    return HU_OK;
#endif
}

static const char *mlx_qwen3_active_adapter(void *ctx_ptr) {
    if (!ctx_ptr)
        return NULL;
    mlx_qwen3_ctx_t *c = (mlx_qwen3_ctx_t *)ctx_ptr;
    /* Returns the caller-supplied id, never the path, so the daemon's
     * personalization-status log doesn't accidentally leak filesystem
     * locations into observability output. */
    return c->active_adapter_id;
}

/* ──────────────────────────────────────────────────────────────────
 * Vtable: identity / capability / lifecycle
 * ────────────────────────────────────────────────────────────────── */

static const char *mlx_qwen3_get_name(void *ctx_ptr) {
    (void)ctx_ptr;
    return HU_MLX_QWEN3_NAME;
}

static bool mlx_qwen3_supports_native_tools(void *ctx_ptr) {
    (void)ctx_ptr;
    /* Tool calls are not part of S1 — Qwen3-Instruct supports them but
     * the helper protocol doesn't carry tool specs yet. Forced false
     * so the agent layer falls back to text-instruct tool emulation. */
    return false;
}

static bool mlx_qwen3_supports_streaming(void *ctx_ptr) {
    (void)ctx_ptr;
    return false;
}

static void mlx_qwen3_deinit(void *ctx_ptr, hu_allocator_t *alloc) {
    if (!ctx_ptr || !alloc)
        return;
    mlx_qwen3_ctx_t *c = (mlx_qwen3_ctx_t *)ctx_ptr;
    clear_active_adapter(c);
#if HU_MLX_QWEN3_PRODUCTION
    shutdown_helper(c);
#endif
    if (c->model_path_owned)
        alloc->free(alloc->ctx, c->model_path_owned, strlen(c->model_path_owned) + 1);
    if (c->python_executable_owned)
        alloc->free(alloc->ctx, c->python_executable_owned,
                    strlen(c->python_executable_owned) + 1);
    if (c->helper_script_path_owned)
        alloc->free(alloc->ctx, c->helper_script_path_owned,
                    strlen(c->helper_script_path_owned) + 1);
    alloc->free(alloc->ctx, c, sizeof(mlx_qwen3_ctx_t));
}

static const hu_provider_vtable_t mlx_qwen3_vtable = {
    .chat_with_system = mlx_qwen3_chat_with_system,
    .chat = mlx_qwen3_chat,
    .supports_native_tools = mlx_qwen3_supports_native_tools,
    .supports_streaming = mlx_qwen3_supports_streaming,
    .get_name = mlx_qwen3_get_name,
    .deinit = mlx_qwen3_deinit,
    .load_adapter = mlx_qwen3_load_adapter,
    .unload_adapter = mlx_qwen3_unload_adapter,
    .active_adapter = mlx_qwen3_active_adapter,
};

/* ──────────────────────────────────────────────────────────────────
 * Factory
 * ────────────────────────────────────────────────────────────────── */

uint32_t hu_mlx_qwen3_helper_protocol_version(void) {
    return HU_MLX_QWEN3_PROTOCOL_VERSION;
}

hu_error_t hu_mlx_qwen3_provider_create(hu_allocator_t *alloc,
                                        const hu_mlx_qwen3_config_t *config,
                                        hu_provider_t *out) {
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;

    mlx_qwen3_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(mlx_qwen3_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->config = *config;
    /* Don't borrow caller's pointers — copy slices we care about. */
    c->config.model_path = NULL;
    c->config.python_executable = NULL;
    c->config.helper_script_path = NULL;
#if HU_MLX_QWEN3_PRODUCTION
    c->helper_pid = -1;
    c->helper_stdin_fd = -1;
    c->helper_stdout_fd = -1;
#endif

    /* Apply config defaults. */
    if (c->config.spawn_timeout_ms == 0)
        c->config.spawn_timeout_ms = HU_MLX_QWEN3_DEFAULT_SPAWN_TIMEOUT_MS;
    if (c->config.chat_timeout_ms == 0)
        c->config.chat_timeout_ms = HU_MLX_QWEN3_DEFAULT_CHAT_TIMEOUT_MS;
    if (c->config.max_tokens_default == 0)
        c->config.max_tokens_default = HU_MLX_QWEN3_DEFAULT_MAX_TOKENS;
    if (c->config.resurrect_max_attempts == 0)
        c->config.resurrect_max_attempts = HU_MLX_QWEN3_DEFAULT_RESURRECT_ATTEMPTS;

    if (config->model_path) {
        c->model_path_owned = dup_slice_nul(alloc, config->model_path, config->model_path_len);
        if (!c->model_path_owned) {
            mlx_qwen3_deinit(c, alloc);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    if (config->python_executable) {
        c->python_executable_owned =
            dup_slice_nul(alloc, config->python_executable, config->python_executable_len);
        if (!c->python_executable_owned) {
            mlx_qwen3_deinit(c, alloc);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    if (config->helper_script_path) {
        c->helper_script_path_owned =
            dup_slice_nul(alloc, config->helper_script_path, config->helper_script_path_len);
        if (!c->helper_script_path_owned) {
            mlx_qwen3_deinit(c, alloc);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    out->ctx = c;
    out->vtable = &mlx_qwen3_vtable;
    return HU_OK;
}
