/* Minimal WASI syscall declarations for SeaClaw WASM build.
 * Matches wasi_snapshot_preview1 ABI.
 * Include only when __wasi__ is defined. */
#ifndef SC_WASI_SYSCALLS_H
#define SC_WASI_SYSCALLS_H

#ifdef __wasi__

#include <stddef.h>
#include <stdint.h>

typedef uint16_t __wasi_errno_t;
typedef uint32_t __wasi_clockid_t;
typedef uint64_t __wasi_timestamp_t;
typedef uint32_t __wasi_size_t;
typedef int __wasi_fd_t;
typedef uint64_t __wasi_rights_t;
typedef uint16_t __wasi_lookupflags_t;
typedef uint16_t __wasi_oflags_t;
typedef uint16_t __wasi_fdflags_t;
typedef uint8_t __wasi_exitcode_t;

#define __WASI_CLOCKID_REALTIME 0
#define __WASI_ERRNO_SUCCESS 0
#define __WASI_LOOKUPFLAGS_SYMLINK_FOLLOW (1u << 0)
#define __WASI_OFLAGS_CREAT (1u << 0)
#define __WASI_RIGHTS_FD_READ (1ULL << 1)
#define __WASI_RIGHTS_FD_WRITE (1ULL << 6)
#define __WASI_RIGHTS_PATH_OPEN (1ULL << 13)
#define __WASI_RIGHTS_ALL (0x3FFFFFFFFFFFFFFFULL)

typedef struct {
    uint8_t *buf;
    __wasi_size_t buf_len;
} __wasi_iovec_t;

typedef struct {
    const uint8_t *buf;
    __wasi_size_t buf_len;
} __wasi_ciovec_t;

#if defined(__clang__)
#define SC_WASI_IMPORT(name) __attribute__((import_module("wasi_snapshot_preview1"), import_name(name)))
#else
#define SC_WASI_IMPORT(name)
#endif

/* From wasi_snapshot_preview1 */
__wasi_errno_t __wasi_fd_read(__wasi_fd_t fd, const __wasi_iovec_t *iovs,
    size_t iovs_len, __wasi_size_t *retptr0) SC_WASI_IMPORT("fd_read");
__wasi_errno_t __wasi_fd_write(__wasi_fd_t fd, const __wasi_ciovec_t *iovs,
    size_t iovs_len, __wasi_size_t *retptr0) SC_WASI_IMPORT("fd_write");
__wasi_errno_t __wasi_fd_close(__wasi_fd_t fd) SC_WASI_IMPORT("fd_close");
__wasi_errno_t __wasi_path_open(__wasi_fd_t fd, __wasi_lookupflags_t dirflags,
    const char *path, __wasi_oflags_t oflags,
    __wasi_rights_t fs_rights_base, __wasi_rights_t fs_rights_inheriting,
    __wasi_fdflags_t fdflags, __wasi_fd_t *retptr0) SC_WASI_IMPORT("path_open");
__wasi_errno_t __wasi_clock_time_get(__wasi_clockid_t id,
    __wasi_timestamp_t precision, __wasi_timestamp_t *retptr0) SC_WASI_IMPORT("clock_time_get");
__wasi_errno_t __wasi_random_get(uint8_t *buf, __wasi_size_t buf_len) SC_WASI_IMPORT("random_get");
__wasi_errno_t __wasi_environ_get(uint8_t **environ, uint8_t *environ_buf) SC_WASI_IMPORT("environ_get");
__wasi_errno_t __wasi_environ_sizes_get(__wasi_size_t *retptr0, __wasi_size_t *retptr1) SC_WASI_IMPORT("environ_sizes_get");
__wasi_errno_t __wasi_args_get(uint8_t **argv, uint8_t *argv_buf) SC_WASI_IMPORT("args_get");
__wasi_errno_t __wasi_args_sizes_get(__wasi_size_t *retptr0, __wasi_size_t *retptr1) SC_WASI_IMPORT("args_sizes_get");
_Noreturn void __wasi_proc_exit(__wasi_exitcode_t rval) SC_WASI_IMPORT("proc_exit");

#endif /* __wasi__ */

#endif /* SC_WASI_SYSCALLS_H */
