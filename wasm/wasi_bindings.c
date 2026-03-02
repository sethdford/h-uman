/* WASI binding wrappers for SeaClaw WASM target. */
#ifdef __wasi__

#include "seaclaw/wasm/wasi_bindings.h"
#include "seaclaw/wasm/wasi_syscalls.h"
#include <string.h>

#define SC_WASI_STDIN  0
#define SC_WASI_STDOUT 1
#define SC_WASI_STDERR 2
#define SC_WASI_PREOPEN_ROOT 3

/* File I/O */
int sc_wasi_fd_read(int fd, void *buf, size_t len, size_t *nread) {
    __wasi_iovec_t iov = { .buf = (uint8_t *)buf, .buf_len = len };
    __wasi_size_t n = 0;
    __wasi_errno_t err = __wasi_fd_read((__wasi_fd_t)fd, &iov, 1, &n);
    if (nread) *nread = n;
    return (int)err;
}

int sc_wasi_fd_write(int fd, const void *buf, size_t len, size_t *nwritten) {
    __wasi_ciovec_t iov = { .buf = (const uint8_t *)buf, .buf_len = len };
    __wasi_size_t n = 0;
    __wasi_errno_t err = __wasi_fd_write((__wasi_fd_t)fd, &iov, 1, &n);
    if (nwritten) *nwritten = n;
    return (int)err;
}

int sc_wasi_fd_close(int fd) {
    return (int)__wasi_fd_close((__wasi_fd_t)fd);
}

int sc_wasi_path_open(int dir_fd, const char *path, int *out_fd) {
    __wasi_fd_t result = 0;
    __wasi_errno_t err = __wasi_path_open(
        (__wasi_fd_t)dir_fd,
        __WASI_LOOKUPFLAGS_SYMLINK_FOLLOW,
        path,
        0,  /* oflags: open existing */
        __WASI_RIGHTS_FD_READ | __WASI_RIGHTS_FD_SEEK,
        __WASI_RIGHTS_ALL,
        0,  /* fdflags */
        &result);
    if (out_fd) *out_fd = (int)result;
    return (int)err;
}

int sc_wasi_file_read_all(void *alloc_ctx, void *(*alloc_fn)(void *, size_t),
    int dir_fd, const char *path, char **buf_out, size_t *len_out)
{
    int fd = -1;
    __wasi_errno_t err = sc_wasi_path_open(dir_fd, path, &fd);
    if (err != 0 || fd < 0) return err;

    char stack_buf[65536];
    size_t total = 0;
    for (;;) {
        size_t nread = 0;
        size_t to_read = sizeof(stack_buf) - total;
        if (to_read == 0) break;
        int r = sc_wasi_fd_read(fd, stack_buf + total, to_read, &nread);
        if (r != 0 || nread == 0) break;
        total += nread;
    }
    sc_wasi_fd_close(fd);

    char *buf = (char *)alloc_fn(alloc_ctx, total + 1);
    if (!buf) return -1;
    memcpy(buf, stack_buf, total);
    buf[total] = '\0';
    *buf_out = buf;
    *len_out = total;
    return 0;
}

/* Clock */
int sc_wasi_clock_time_get_realtime(uint64_t *out_ns) {
    __wasi_timestamp_t t = 0;
    __wasi_errno_t err = __wasi_clock_time_get(
        __WASI_CLOCKID_REALTIME, 1, &t);
    if (out_ns) *out_ns = t;
    return (int)err;
}

/* Random */
int sc_wasi_random_get(void *buf, size_t len) {
    return (int)__wasi_random_get((uint8_t *)buf, (__wasi_size_t)len);
}

/* Environment */
int sc_wasi_environ_sizes_get(size_t *env_count, size_t *env_buf_len) {
    __wasi_size_t c = 0, b = 0;
    __wasi_errno_t err = __wasi_environ_sizes_get(&c, &b);
    if (env_count) *env_count = c;
    if (env_buf_len) *env_buf_len = b;
    return (int)err;
}

int sc_wasi_environ_get(char **env_ptrs, char *env_buf) {
    return (int)__wasi_environ_get((uint8_t **)env_ptrs, (uint8_t *)env_buf);
}

/* Args */
int sc_wasi_args_sizes_get(size_t *argc, size_t *argv_buf_len) {
    __wasi_size_t c = 0, b = 0;
    __wasi_errno_t err = __wasi_args_sizes_get(&c, &b);
    if (argc) *argc = c;
    if (argv_buf_len) *argv_buf_len = b;
    return (int)err;
}

int sc_wasi_args_get(char **argv_ptrs, char *argv_buf) {
    return (int)__wasi_args_get((uint8_t **)argv_ptrs, (uint8_t *)argv_buf);
}

/* Process */
void sc_wasi_proc_exit(int code) {
    __wasi_proc_exit((__wasi_exitcode_t)code);
}

#endif /* __wasi__ */
