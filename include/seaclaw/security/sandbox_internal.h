#ifndef SC_SANDBOX_INTERNAL_H
#define SC_SANDBOX_INTERNAL_H

#include "seaclaw/security/sandbox.h"
#include <stdint.h>

/* Internal types and init — only for detect.c */

typedef struct { char _; } sc_noop_sandbox_ctx_t;
sc_sandbox_t sc_noop_sandbox_get(sc_noop_sandbox_ctx_t *ctx);

typedef struct { char workspace_dir[1024]; } sc_landlock_ctx_t;
void sc_landlock_sandbox_init(sc_landlock_ctx_t *ctx, const char *workspace_dir);
sc_sandbox_t sc_landlock_sandbox_get(sc_landlock_ctx_t *ctx);

typedef struct {
    char private_arg[256];
    size_t private_len;
    const char *const *extra_args;
    size_t extra_args_len;
} sc_firejail_ctx_t;
void sc_firejail_sandbox_init(sc_firejail_ctx_t *ctx, const char *workspace_dir);
void sc_firejail_sandbox_set_extra_args(sc_firejail_ctx_t *ctx,
    const char *const *args, size_t args_len);
sc_sandbox_t sc_firejail_sandbox_get(sc_firejail_ctx_t *ctx);

typedef struct { char workspace_dir[2048]; } sc_bubblewrap_ctx_t;
void sc_bubblewrap_sandbox_init(sc_bubblewrap_ctx_t *ctx, const char *workspace_dir);
sc_sandbox_t sc_bubblewrap_sandbox_get(sc_bubblewrap_ctx_t *ctx);

typedef struct {
    void *alloc_ctx;
    void *(*alloc_fn)(void *, size_t);
    void (*free_fn)(void *, void *, size_t);
    char mount_arg[4097];
    size_t mount_len;
    char image[128];
} sc_docker_ctx_t;
void sc_docker_sandbox_init(sc_docker_ctx_t *ctx, const char *workspace_dir,
    const char *image, void *alloc_ctx,
    void *(*alloc_fn)(void *, size_t),
    void (*free_fn)(void *, void *, size_t));
sc_sandbox_t sc_docker_sandbox_get(sc_docker_ctx_t *ctx);

/* --- Seatbelt (macOS) --- */
typedef struct {
    char workspace_dir[1024];
    char profile[2048];
    size_t profile_len;
} sc_seatbelt_ctx_t;
void sc_seatbelt_sandbox_init(sc_seatbelt_ctx_t *ctx, const char *workspace_dir);
sc_sandbox_t sc_seatbelt_sandbox_get(sc_seatbelt_ctx_t *ctx);

/* --- seccomp-BPF (Linux) --- */
typedef struct {
    char workspace_dir[1024];
    bool allow_network;
} sc_seccomp_ctx_t;
void sc_seccomp_sandbox_init(sc_seccomp_ctx_t *ctx, const char *workspace_dir,
    bool allow_network);
sc_sandbox_t sc_seccomp_sandbox_get(sc_seccomp_ctx_t *ctx);

/* --- Landlock + seccomp combined (Linux) --- */
typedef struct {
    sc_landlock_ctx_t landlock;
    sc_seccomp_ctx_t seccomp;
} sc_landlock_seccomp_ctx_t;
void sc_landlock_seccomp_sandbox_init(sc_landlock_seccomp_ctx_t *ctx,
    const char *workspace_dir, bool allow_network);
sc_sandbox_t sc_landlock_seccomp_sandbox_get(sc_landlock_seccomp_ctx_t *ctx);

/* --- AppContainer (Windows) --- */
typedef struct {
    char workspace_dir[1024];
    char app_name[128];
} sc_appcontainer_ctx_t;
void sc_appcontainer_sandbox_init(sc_appcontainer_ctx_t *ctx,
    const char *workspace_dir);
sc_sandbox_t sc_appcontainer_sandbox_get(sc_appcontainer_ctx_t *ctx);

/* --- WASI (cross-platform) --- */
typedef struct {
    char workspace_dir[1024];
    char runtime_path[256];
    char dir_arg[1040];
} sc_wasi_sandbox_ctx_t;
void sc_wasi_sandbox_init(sc_wasi_sandbox_ctx_t *ctx, const char *workspace_dir);
sc_sandbox_t sc_wasi_sandbox_get(sc_wasi_sandbox_ctx_t *ctx);

/* --- Firecracker (Linux microVM) --- */
typedef struct {
    char workspace_dir[1024];
    char socket_path[256];
    char kernel_path[256];
    char rootfs_path[256];
    uint32_t vcpu_count;
    uint32_t mem_size_mib;
} sc_firecracker_ctx_t;
void sc_firecracker_sandbox_init(sc_firecracker_ctx_t *ctx,
    const char *workspace_dir);
sc_sandbox_t sc_firecracker_sandbox_get(sc_firecracker_ctx_t *ctx);

#endif /* SC_SANDBOX_INTERNAL_H */
