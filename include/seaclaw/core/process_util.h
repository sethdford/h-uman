#ifndef SC_PROCESS_UTIL_H
#define SC_PROCESS_UTIL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct sc_run_result {
    char *stdout_buf;
    size_t stdout_len;
    size_t stdout_cap;  /* allocated capacity for free() */
    char *stderr_buf;
    size_t stderr_len;
    size_t stderr_cap;  /* allocated capacity for free() */
    bool success;
    int exit_code;  /* -1 if terminated by signal */
} sc_run_result_t;

void sc_run_result_free(sc_allocator_t *alloc, sc_run_result_t *r);

/**
 * Callback invoked in child process after fork, before the command runs.
 * Used by kernel-level sandboxes (Landlock, seccomp) to apply restrictions.
 * Return SC_OK to proceed, or SC_ERR_* to abort (child exits with code 125).
 */
typedef sc_error_t (*sc_child_setup_fn)(void *ctx);

/**
 * Run a child process, capture stdout and stderr.
 * Caller must call sc_run_result_free on the result.
 * argv[0] is the program, argv[argc] must be NULL.
 *
 * @param alloc Allocator for buffers
 * @param argv NULL-terminated argv (argv[0]=program, argv[argc]=NULL)
 * @param cwd Working directory (NULL = inherit)
 * @param max_output_bytes Max bytes to capture per stream (default 1MB)
 * @param out Result; caller must free with sc_run_result_free
 */
sc_error_t sc_process_run(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_run_result_t *out);

/**
 * Run a child process with an optional pre-run callback.
 * child_setup is called in the child after fork() but before the command runs.
 * If child_setup is NULL, behaves identically to sc_process_run.
 */
sc_error_t sc_process_run_sandboxed(sc_allocator_t *alloc,
    const char *const *argv,
    const char *cwd,
    size_t max_output_bytes,
    sc_child_setup_fn child_setup,
    void *child_setup_ctx,
    sc_run_result_t *out);

#endif /* SC_PROCESS_UTIL_H */
