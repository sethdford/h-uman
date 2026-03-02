#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>

/*
 * seccomp-BPF syscall filter sandbox (Linux only).
 *
 * Installs a BPF filter that restricts the set of syscalls available to the
 * child process. Combined with Landlock (filesystem ACLs), this provides
 * Chrome-grade process isolation.
 *
 * Policy:
 *  - Allow common safe syscalls (read, write, open, close, mmap, etc.)
 *  - Block dangerous syscalls (ptrace, mount, reboot, kexec, etc.)
 *  - Optionally block network syscalls (socket, connect, bind, etc.)
 *
 * Uses the kernel seccomp() syscall directly to avoid libseccomp dependency.
 */

#ifdef __linux__
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#if defined(__x86_64__)
#define SC_SECCOMP_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SC_SECCOMP_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define SC_SECCOMP_AUDIT_ARCH AUDIT_ARCH_I386
#else
#define SC_SECCOMP_AUDIT_ARCH 0
#endif

#ifndef SECCOMP_RET_ALLOW
#define SECCOMP_RET_ALLOW 0x7fff0000U
#endif
#ifndef SECCOMP_RET_ERRNO
#define SECCOMP_RET_ERRNO 0x00050000U
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

static bool seccomp_supported(void) {
#if SC_IS_TEST
    return false;
#else
    /* prctl(PR_GET_SECCOMP) returns:
     *   0     = seccomp supported, not active
     *   2     = seccomp active (filter mode)
     *  -1/EINVAL = kernel doesn't know about seccomp → not supported
     *  -1/other  = seccomp known but query failed → assume supported
     */
    int r = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    if (r >= 0) return true;
    return errno != EINVAL;
#endif
}

/* BPF instruction helpers */
#define BPF_STMT_SC(code, k) ((struct sock_filter){(unsigned short)(code), 0, 0, (unsigned int)(k)})
#define BPF_JUMP_SC(code, k, jt, jf) ((struct sock_filter){(unsigned short)(code), (jt), (jf), (unsigned int)(k)})

/*
 * Install seccomp-BPF filter. Called after fork(), before child runs.
 * Block dangerous syscalls; optionally block networking.
 */
static sc_error_t seccomp_apply(void *ctx) {
#if SC_IS_TEST
    (void)ctx;
    return SC_OK;
#else
    sc_seccomp_ctx_t *sc = (sc_seccomp_ctx_t *)ctx;

    /* Dangerous syscalls to block unconditionally (return EPERM) */
    static const int blocked_syscalls[] = {
        SYS_ptrace,
        SYS_mount,
#ifdef SYS_umount2
        SYS_umount2,
#endif
        SYS_reboot,
#ifdef SYS_kexec_load
        SYS_kexec_load,
#endif
#ifdef SYS_init_module
        SYS_init_module,
#endif
#ifdef SYS_delete_module
        SYS_delete_module,
#endif
#ifdef SYS_pivot_root
        SYS_pivot_root,
#endif
#ifdef SYS_swapon
        SYS_swapon,
#endif
#ifdef SYS_swapoff
        SYS_swapoff,
#endif
#ifdef SYS_acct
        SYS_acct,
#endif
#ifdef SYS_unshare
        SYS_unshare,
#endif
#ifdef SYS_setns
        SYS_setns,
#endif
#ifdef SYS_add_key
        SYS_add_key,
#endif
#ifdef SYS_request_key
        SYS_request_key,
#endif
#ifdef SYS_keyctl
        SYS_keyctl,
#endif
#ifdef SYS_perf_event_open
        SYS_perf_event_open,
#endif
#ifdef SYS_bpf
        SYS_bpf,
#endif
#ifdef SYS_userfaultfd
        SYS_userfaultfd,
#endif
    };

    /* Network syscalls to block when allow_network is false */
    static const int network_syscalls[] = {
        SYS_socket,
        SYS_connect,
        SYS_bind,
        SYS_listen,
        SYS_accept,
#ifdef SYS_accept4
        SYS_accept4,
#endif
        SYS_sendto,
        SYS_recvfrom,
        SYS_sendmsg,
        SYS_recvmsg,
    };

    const size_t n_blocked = sizeof(blocked_syscalls) / sizeof(blocked_syscalls[0]);
    const size_t n_network = sc->allow_network ? 0
        : sizeof(network_syscalls) / sizeof(network_syscalls[0]);

    /*
     * BPF program layout:
     *   [0]     Load arch
     *   [1]     Check arch (kill if wrong)
     *   [2]     Load syscall nr
     *   [3..N]  Check blocked syscalls -> ERRNO(EPERM)
     *   [N+1..M] Check network syscalls -> ERRNO(EPERM)
     *   [M+1]   ALLOW
     */
    const size_t max_insns = 3 + n_blocked * 2 + n_network * 2 + 1;
    struct sock_filter filter[128];
    if (max_insns > sizeof(filter) / sizeof(filter[0]))
        return SC_ERR_INTERNAL;

    size_t idx = 0;

    /* Load architecture */
    filter[idx++] = BPF_STMT_SC(BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, arch));
    /* Verify architecture matches */
    filter[idx++] = BPF_JUMP_SC(BPF_JMP | BPF_JEQ | BPF_K,
        SC_SECCOMP_AUDIT_ARCH, 1, 0);
    filter[idx++] = BPF_STMT_SC(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* Load syscall number */
    filter[idx++] = BPF_STMT_SC(BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));

    /* Block dangerous syscalls */
    for (size_t i = 0; i < n_blocked; i++) {
        filter[idx++] = BPF_JUMP_SC(BPF_JMP | BPF_JEQ | BPF_K,
            (unsigned int)blocked_syscalls[i], 0, 1);
        filter[idx++] = BPF_STMT_SC(BPF_RET | BPF_K,
            SECCOMP_RET_ERRNO | (EPERM & 0xFFFF));
    }

    /* Block network syscalls if not allowed */
    for (size_t i = 0; i < n_network; i++) {
        filter[idx++] = BPF_JUMP_SC(BPF_JMP | BPF_JEQ | BPF_K,
            (unsigned int)network_syscalls[i], 0, 1);
        filter[idx++] = BPF_STMT_SC(BPF_RET | BPF_K,
            SECCOMP_RET_ERRNO | (EPERM & 0xFFFF));
    }

    /* Allow everything else */
    filter[idx++] = BPF_STMT_SC(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    struct sock_fprog prog = {
        .len = (unsigned short)idx,
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return SC_ERR_IO;

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) < 0)
        return SC_ERR_IO;

    return SC_OK;
#endif /* SC_IS_TEST */
}
#endif /* __linux__ */

static sc_error_t seccomp_wrap(void *ctx, const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx; (void)argv; (void)argc; (void)buf; (void)buf_count; (void)out_count;
    return SC_ERR_NOT_SUPPORTED;
#else
    (void)ctx;
    if (!buf || !out_count) return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < argc) return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < argc; i++)
        buf[i] = argv[i];
    *out_count = argc;
    return SC_OK;
#endif
}

static bool seccomp_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
    return seccomp_supported();
#else
    return false;
#endif
}

static const char *seccomp_name(void *ctx) {
    (void)ctx;
    return "seccomp";
}

static const char *seccomp_desc(void *ctx) {
    (void)ctx;
#ifdef __linux__
    return "Linux seccomp-BPF syscall filter (kernel-level, near-zero overhead)";
#else
    return "Linux seccomp-BPF (not available on this platform)";
#endif
}

static const sc_sandbox_vtable_t seccomp_vtable = {
    .wrap_command = seccomp_wrap,
#ifdef __linux__
    .apply = seccomp_apply,
#else
    .apply = NULL,
#endif
    .is_available = seccomp_available,
    .name = seccomp_name,
    .description = seccomp_desc,
};

sc_sandbox_t sc_seccomp_sandbox_get(sc_seccomp_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &seccomp_vtable,
    };
    return sb;
}

void sc_seccomp_sandbox_init(sc_seccomp_ctx_t *ctx, const char *workspace_dir,
    bool allow_network) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->allow_network = allow_network;
    if (workspace_dir) {
        size_t len = strlen(workspace_dir);
        if (len >= sizeof(ctx->workspace_dir))
            len = sizeof(ctx->workspace_dir) - 1;
        memcpy(ctx->workspace_dir, workspace_dir, len);
        ctx->workspace_dir[len] = '\0';
    }
}
