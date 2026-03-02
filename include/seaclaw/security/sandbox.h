#ifndef SC_SANDBOX_H
#define SC_SANDBOX_H

#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Sandbox vtable interface for OS-level isolation. */

typedef struct sc_sandbox sc_sandbox_t;

/**
 * Wrap a command with sandbox protection.
 * argv/argc: original command; buf/buf_count: output buffer for wrapped argv.
 * Returns SC_OK and sets *out_count, or SC_ERR_* on error.
 */
typedef sc_error_t (*sc_sandbox_wrap_fn)(void *ctx,
    const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count);

/**
 * Apply sandbox restrictions to the current process (called after fork,
 * before the child runs). Used by kernel-level sandboxes (Landlock, seccomp)
 * that cannot be applied via argv wrapping. Returns SC_OK or SC_ERR_*.
 * NULL means not applicable — use wrap_command instead.
 */
typedef sc_error_t (*sc_sandbox_apply_fn)(void *ctx);

typedef bool (*sc_sandbox_available_fn)(void *ctx);
typedef const char *(*sc_sandbox_name_fn)(void *ctx);
typedef const char *(*sc_sandbox_desc_fn)(void *ctx);

typedef struct sc_sandbox_vtable {
    sc_sandbox_wrap_fn wrap_command;
    sc_sandbox_apply_fn apply;
    sc_sandbox_available_fn is_available;
    sc_sandbox_name_fn name;
    sc_sandbox_desc_fn description;
} sc_sandbox_vtable_t;

struct sc_sandbox {
    void *ctx;
    const sc_sandbox_vtable_t *vtable;
};

static inline sc_error_t sc_sandbox_wrap_command(sc_sandbox_t *sb,
    const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
    if (!sb || !sb->vtable || !sb->vtable->wrap_command) return SC_ERR_INVALID_ARGUMENT;
    return sb->vtable->wrap_command(sb->ctx, argv, argc, buf, buf_count, out_count);
}

static inline sc_error_t sc_sandbox_apply(sc_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->apply) return SC_OK;
    return sb->vtable->apply(sb->ctx);
}

static inline bool sc_sandbox_is_available(sc_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->is_available) return false;
    return sb->vtable->is_available(sb->ctx);
}

static inline const char *sc_sandbox_name(sc_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->name) return "none";
    return sb->vtable->name(sb->ctx);
}

static inline const char *sc_sandbox_description(sc_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->description) return "";
    return sb->vtable->description(sb->ctx);
}

/* Backend preference */
typedef enum sc_sandbox_backend {
    SC_SANDBOX_AUTO,
    SC_SANDBOX_NONE,
    SC_SANDBOX_LANDLOCK,
    SC_SANDBOX_FIREJAIL,
    SC_SANDBOX_BUBBLEWRAP,
    SC_SANDBOX_DOCKER,
    SC_SANDBOX_SEATBELT,
    SC_SANDBOX_SECCOMP,
    SC_SANDBOX_WASI,
    SC_SANDBOX_FIRECRACKER,
} sc_sandbox_backend_t;

/* Allocator interface for docker sandbox */
typedef struct sc_sandbox_alloc {
    void *ctx;
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr, size_t size);
} sc_sandbox_alloc_t;

/* Storage for createSandbox (allocated by library, holds backend instances) */
typedef struct sc_sandbox_storage sc_sandbox_storage_t;

sc_sandbox_storage_t *sc_sandbox_storage_create(const sc_sandbox_alloc_t *alloc);
void sc_sandbox_storage_destroy(sc_sandbox_storage_t *s,
    const sc_sandbox_alloc_t *alloc);

/** Create sandbox. Storage must remain valid for lifetime of returned sandbox. */
sc_sandbox_t sc_sandbox_create(sc_sandbox_backend_t backend,
    const char *workspace_dir,
    sc_sandbox_storage_t *storage,
    const sc_sandbox_alloc_t *alloc);

typedef struct sc_available_backends {
    bool landlock;
    bool firejail;
    bool bubblewrap;
    bool docker;
    bool seatbelt;
    bool seccomp;
    bool wasi;
    bool firecracker;
} sc_available_backends_t;

sc_available_backends_t sc_sandbox_detect_available(const char *workspace_dir,
    const sc_sandbox_alloc_t *alloc);

/** Create a noop sandbox (no isolation). Zig parity: createNoopSandbox. */
sc_sandbox_t sc_sandbox_create_noop(void);

#endif /* SC_SANDBOX_H */
