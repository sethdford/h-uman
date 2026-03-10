#ifndef HU_SANDBOX_INTERNAL_H
#define HU_SANDBOX_INTERNAL_H

#include "human/security/sandbox.h"
#include <stdint.h>

/* Internal types and init — only for detect.c */

typedef struct {
    char _;
} hu_noop_sandbox_ctx_t;
hu_sandbox_t hu_noop_sandbox_get(hu_noop_sandbox_ctx_t *ctx);

typedef struct {
    char workspace_dir[1024];
} hu_landlock_ctx_t;
void hu_landlock_sandbox_init(hu_landlock_ctx_t *ctx, const char *workspace_dir);
hu_sandbox_t hu_landlock_sandbox_get(hu_landlock_ctx_t *ctx);

typedef struct {
    char private_arg[256];
    size_t private_len;
    const char *const *extra_args;
    size_t extra_args_len;
} hu_firejail_ctx_t;
void hu_firejail_sandbox_init(hu_firejail_ctx_t *ctx, const char *workspace_dir);
void hu_firejail_sandbox_set_extra_args(hu_firejail_ctx_t *ctx, const char *const *args,
                                        size_t args_len);
hu_sandbox_t hu_firejail_sandbox_get(hu_firejail_ctx_t *ctx);

typedef struct {
    char workspace_dir[2048];
} hu_bubblewrap_ctx_t;
void hu_bubblewrap_sandbox_init(hu_bubblewrap_ctx_t *ctx, const char *workspace_dir);
hu_sandbox_t hu_bubblewrap_sandbox_get(hu_bubblewrap_ctx_t *ctx);

typedef struct {
    void *alloc_ctx;
    void *(*alloc_fn)(void *, size_t);
    void (*free_fn)(void *, void *, size_t);
    char mount_arg[4097];
    size_t mount_len;
    char image[128];
} hu_docker_ctx_t;
void hu_docker_sandbox_init(hu_docker_ctx_t *ctx, const char *workspace_dir, const char *image,
                            void *alloc_ctx, void *(*alloc_fn)(void *, size_t),
                            void (*free_fn)(void *, void *, size_t));
hu_sandbox_t hu_docker_sandbox_get(hu_docker_ctx_t *ctx);

/* --- Seatbelt (macOS) --- */
typedef struct {
    char workspace_dir[1024];
    char profile[2048];
    size_t profile_len;
} hu_seatbelt_ctx_t;
void hu_seatbelt_sandbox_init(hu_seatbelt_ctx_t *ctx, const char *workspace_dir);
hu_sandbox_t hu_seatbelt_sandbox_get(hu_seatbelt_ctx_t *ctx);

/* --- seccomp-BPF (Linux) --- */
typedef struct {
    char workspace_dir[1024];
    bool allow_network;
} hu_seccomp_ctx_t;
void hu_seccomp_sandbox_init(hu_seccomp_ctx_t *ctx, const char *workspace_dir, bool allow_network);
hu_sandbox_t hu_seccomp_sandbox_get(hu_seccomp_ctx_t *ctx);

/* --- Landlock + seccomp combined (Linux) --- */
typedef struct {
    hu_landlock_ctx_t landlock;
    hu_seccomp_ctx_t seccomp;
} hu_landlock_seccomp_ctx_t;
void hu_landlock_seccomp_sandbox_init(hu_landlock_seccomp_ctx_t *ctx, const char *workspace_dir,
                                      bool allow_network);
hu_sandbox_t hu_landlock_seccomp_sandbox_get(hu_landlock_seccomp_ctx_t *ctx);

/* --- AppContainer (Windows) --- */
typedef struct {
    char workspace_dir[1024];
    char app_name[128];
} hu_appcontainer_ctx_t;
void hu_appcontainer_sandbox_init(hu_appcontainer_ctx_t *ctx, const char *workspace_dir);
hu_sandbox_t hu_appcontainer_sandbox_get(hu_appcontainer_ctx_t *ctx);

/* --- WASI (cross-platform) --- */
typedef struct {
    char workspace_dir[1024];
    char runtime_path[256];
    char dir_arg[1040];
} hu_wasi_sandbox_ctx_t;
void hu_wasi_sandbox_init(hu_wasi_sandbox_ctx_t *ctx, const char *workspace_dir);
hu_sandbox_t hu_wasi_sandbox_get(hu_wasi_sandbox_ctx_t *ctx);

/* --- Firecracker (Linux microVM) --- */
typedef struct {
    char workspace_dir[1024];
    char socket_path[256];
    char kernel_path[256];
    char rootfs_path[256];
    uint32_t vcpu_count;
    uint32_t mem_size_mib;
} hu_firecracker_ctx_t;
void hu_firecracker_sandbox_init(hu_firecracker_ctx_t *ctx, const char *workspace_dir);
hu_sandbox_t hu_firecracker_sandbox_get(hu_firecracker_ctx_t *ctx);

#endif /* HU_SANDBOX_INTERNAL_H */
