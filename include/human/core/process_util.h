#ifndef HU_PROCESS_UTIL_H
#define HU_PROCESS_UTIL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_run_result {
    char *stdout_buf;
    size_t stdout_len;
    size_t stdout_cap; /* allocated capacity for free() */
    char *stderr_buf;
    size_t stderr_len;
    size_t stderr_cap; /* allocated capacity for free() */
    bool success;
    int exit_code; /* -1 if terminated by signal */
} hu_run_result_t;

void hu_run_result_free(hu_allocator_t *alloc, hu_run_result_t *r);

/**
 * Callback invoked in child process after fork, before the command runs.
 * Used by kernel-level sandboxes (Landlock, seccomp) to apply restrictions.
 * Return HU_OK to proceed, or HU_ERR_* to abort (child exits with code 125).
 */
typedef hu_error_t (*hu_child_setup_fn)(void *ctx);

/**
 * Run a child process, capture stdout and stderr.
 * Caller must call hu_run_result_free on the result.
 * argv[0] is the program, argv[argc] must be NULL.
 *
 * @param alloc Allocator for buffers
 * @param argv NULL-terminated argv (argv[0]=program, argv[argc]=NULL)
 * @param cwd Working directory (NULL = inherit)
 * @param max_output_bytes Max bytes to capture per stream (default 1MB)
 * @param out Result; caller must free with hu_run_result_free
 */
hu_error_t hu_process_run(hu_allocator_t *alloc, const char *const *argv, const char *cwd,
                          size_t max_output_bytes, hu_run_result_t *out);

/**
 * Run a child process with an optional pre-run callback.
 * child_setup is called in the child after fork() but before the command runs.
 * If child_setup is NULL, behaves identically to hu_process_run.
 */
hu_error_t hu_process_run_sandboxed(hu_allocator_t *alloc, const char *const *argv, const char *cwd,
                                    size_t max_output_bytes, hu_child_setup_fn child_setup,
                                    void *child_setup_ctx, hu_run_result_t *out);

/**
 * Run a child process with security policy (sandbox + net_proxy).
 * Applies net_proxy env vars and sandbox restrictions in the child.
 * If policy is NULL, behaves identically to hu_process_run.
 */
hu_error_t hu_process_run_with_policy(hu_allocator_t *alloc, const char *const *argv,
                                      const char *cwd, size_t max_output_bytes,
                                      hu_security_policy_t *policy, hu_run_result_t *out);

#endif /* HU_PROCESS_UTIL_H */
