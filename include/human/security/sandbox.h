#ifndef HU_SANDBOX_H
#define HU_SANDBOX_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

/* Sandbox vtable interface for OS-level isolation. */

typedef struct hu_sandbox hu_sandbox_t;

/**
 * Wrap a command with sandbox protection.
 * argv/argc: original command; buf/buf_count: output buffer for wrapped argv.
 * Returns HU_OK and sets *out_count, or HU_ERR_* on error.
 */
typedef hu_error_t (*hu_sandbox_wrap_fn)(void *ctx, const char *const *argv, size_t argc,
                                         const char **buf, size_t buf_count, size_t *out_count);

/**
 * Apply sandbox restrictions to the current process (called after fork,
 * before the child runs). Used by kernel-level sandboxes (Landlock, seccomp)
 * that cannot be applied via argv wrapping. Returns HU_OK or HU_ERR_*.
 * NULL means not applicable — use wrap_command instead.
 */
typedef hu_error_t (*hu_sandbox_apply_fn)(void *ctx);

typedef bool (*hu_sandbox_available_fn)(void *ctx);
typedef const char *(*hu_sandbox_name_fn)(void *ctx);
typedef const char *(*hu_sandbox_desc_fn)(void *ctx);

typedef struct hu_sandbox_vtable {
    hu_sandbox_wrap_fn wrap_command;
    hu_sandbox_apply_fn apply;
    hu_sandbox_available_fn is_available;
    hu_sandbox_name_fn name;
    hu_sandbox_desc_fn description;
} hu_sandbox_vtable_t;

struct hu_sandbox {
    void *ctx;
    const hu_sandbox_vtable_t *vtable;
};

static inline hu_error_t hu_sandbox_wrap_command(hu_sandbox_t *sb, const char *const *argv,
                                                 size_t argc, const char **buf, size_t buf_count,
                                                 size_t *out_count) {
    if (!sb || !sb->vtable || !sb->vtable->wrap_command)
        return HU_ERR_INVALID_ARGUMENT;
    return sb->vtable->wrap_command(sb->ctx, argv, argc, buf, buf_count, out_count);
}

static inline hu_error_t hu_sandbox_apply(hu_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->apply)
        return HU_OK;
    return sb->vtable->apply(sb->ctx);
}

static inline bool hu_sandbox_is_available(hu_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->is_available)
        return false;
    return sb->vtable->is_available(sb->ctx);
}

static inline const char *hu_sandbox_name(hu_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->name)
        return "none";
    return sb->vtable->name(sb->ctx);
}

static inline const char *hu_sandbox_description(hu_sandbox_t *sb) {
    if (!sb || !sb->vtable || !sb->vtable->description)
        return "";
    return sb->vtable->description(sb->ctx);
}

/* Backend preference */
typedef enum hu_sandbox_backend {
    HU_SANDBOX_AUTO,
    HU_SANDBOX_NONE,
    HU_SANDBOX_LANDLOCK,
    HU_SANDBOX_FIREJAIL,
    HU_SANDBOX_BUBBLEWRAP,
    HU_SANDBOX_DOCKER,
    HU_SANDBOX_SEATBELT,
    HU_SANDBOX_SECCOMP,
    HU_SANDBOX_LANDLOCK_SECCOMP,
    HU_SANDBOX_WASI,
    HU_SANDBOX_FIRECRACKER,
    HU_SANDBOX_APPCONTAINER,
} hu_sandbox_backend_t;

/* Allocator interface for docker sandbox */
typedef struct hu_sandbox_alloc {
    void *ctx;
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr, size_t size);
} hu_sandbox_alloc_t;

/* Storage for createSandbox (allocated by library, holds backend instances) */
typedef struct hu_sandbox_storage hu_sandbox_storage_t;

hu_sandbox_storage_t *hu_sandbox_storage_create(const hu_sandbox_alloc_t *alloc);
void hu_sandbox_storage_destroy(hu_sandbox_storage_t *s, const hu_sandbox_alloc_t *alloc);

/** Create sandbox. Storage must remain valid for lifetime of returned sandbox. */
hu_sandbox_t hu_sandbox_create(hu_sandbox_backend_t backend, const char *workspace_dir,
                               hu_sandbox_storage_t *storage, const hu_sandbox_alloc_t *alloc);

typedef struct hu_available_backends {
    bool landlock;
    bool firejail;
    bool bubblewrap;
    bool docker;
    bool seatbelt;
    bool seccomp;
    bool landlock_seccomp;
    bool wasi;
    bool firecracker;
    bool appcontainer;
} hu_available_backends_t;

hu_available_backends_t hu_sandbox_detect_available(const char *workspace_dir,
                                                    const hu_sandbox_alloc_t *alloc);

/** Create a noop sandbox (no isolation). Zig parity: createNoopSandbox. */
hu_sandbox_t hu_sandbox_create_noop(void);

/* ── Network isolation proxy ──────────────────────────────────────── */

/**
 * Network isolation configuration for sandboxed processes.
 * Composable with any sandbox backend. When attached, child processes
 * route traffic through a filtering proxy that blocks unapproved domains.
 *
 * Usage: set on the sandbox or security policy; the spawn path reads
 * these fields and sets HTTP_PROXY/HTTPS_PROXY environment variables
 * for the child process, pointing to the filtering proxy.
 */
#define HU_NET_PROXY_MAX_DOMAINS 64

typedef struct hu_net_proxy {
    bool enabled;
    bool deny_all;
    const char *proxy_addr;
    const char *allowed_domains[HU_NET_PROXY_MAX_DOMAINS];
    size_t allowed_domains_count;
} hu_net_proxy_t;

/** Check if a domain is allowed by the proxy configuration. */
static inline bool hu_net_proxy_domain_allowed(const hu_net_proxy_t *proxy, const char *domain) {
    if (!proxy || !proxy->enabled)
        return true;
    if (proxy->deny_all && proxy->allowed_domains_count == 0)
        return false;
    if (!domain || !domain[0])
        return false;
    for (size_t i = 0; i < proxy->allowed_domains_count; i++) {
        if (proxy->allowed_domains[i] && strcasecmp(proxy->allowed_domains[i], domain) == 0)
            return true;
        /* Wildcard subdomain matching: *.example.com matches sub.example.com */
        if (proxy->allowed_domains[i] && proxy->allowed_domains[i][0] == '*' &&
            proxy->allowed_domains[i][1] == '.') {
            const char *suffix = proxy->allowed_domains[i] + 1;
            size_t slen = strlen(suffix);
            size_t dlen = strlen(domain);
            if (dlen >= slen && strcasecmp(domain + dlen - slen, suffix) == 0)
                return true;
        }
    }
    return !proxy->deny_all;
}

/** Initialize a deny-all network proxy config. */
static inline void hu_net_proxy_init_deny_all(hu_net_proxy_t *proxy) {
    if (!proxy)
        return;
    memset(proxy, 0, sizeof(*proxy));
    proxy->enabled = true;
    proxy->deny_all = true;
}

/** Add an allowed domain to the proxy config. Returns false if full. */
static inline bool hu_net_proxy_allow_domain(hu_net_proxy_t *proxy, const char *domain) {
    if (!proxy || !domain)
        return false;
    if (proxy->allowed_domains_count >= HU_NET_PROXY_MAX_DOMAINS)
        return false;
    proxy->allowed_domains[proxy->allowed_domains_count++] = domain;
    return true;
}

#endif /* HU_SANDBOX_H */
