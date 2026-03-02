#ifndef SC_WASI_BINDINGS_H
#define SC_WASI_BINDINGS_H

#ifdef __wasi__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* File I/O */
int sc_wasi_fd_read(int fd, void *buf, size_t len, size_t *nread);
int sc_wasi_fd_write(int fd, const void *buf, size_t len, size_t *nwritten);
int sc_wasi_fd_close(int fd);
/* path: path relative to dir_fd (use 3 for preopen /). 0 on success. */
int sc_wasi_path_open(int dir_fd, const char *path, int *out_fd);
/* Read entire file; caller frees buf. Returns 0 on success. */
int sc_wasi_file_read_all(void *alloc_ctx, void *(*alloc_fn)(void *, size_t),
    int dir_fd, const char *path, char **buf_out, size_t *len_out);

/* Clock */
int sc_wasi_clock_time_get_realtime(uint64_t *out_ns);

/* Random */
int sc_wasi_random_get(void *buf, size_t len);

/* Environment */
int sc_wasi_environ_sizes_get(size_t *env_count, size_t *env_buf_len);
int sc_wasi_environ_get(char **env_ptrs, char *env_buf);

/* Args */
int sc_wasi_args_sizes_get(size_t *argc, size_t *argv_buf_len);
int sc_wasi_args_get(char **argv_ptrs, char *argv_buf);

/* Process */
_Noreturn void sc_wasi_proc_exit(int code);

#endif /* __wasi__ */

#endif /* SC_WASI_BINDINGS_H */
