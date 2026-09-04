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
/* Absolute path of the running executable (realpath'd on macOS). Returns
 * false when it cannot be resolved or `cap` is too small; `buf` is then
 * empty. Use this — never a bare "human" on PATH — when a daemon re-execs
 * itself for a subcommand: on 2026-09-02 a stale ~/.local/bin/human from
 * May silently ran the LoRA pair-count probe for weeks. */
bool hu_process_self_exe_path(char *buf, size_t cap);

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

/**
 * Run a child process with a hard timeout (seconds). If the child does not
 * exit within timeout_sec, it is killed with SIGKILL. timeout_sec == 0 means
 * no timeout (identical to hu_process_run). On timeout, exit_code is set to
 * -1 and success to false.
 */
hu_error_t hu_process_run_with_timeout(hu_allocator_t *alloc, const char *const *argv,
                                       const char *cwd, size_t max_output_bytes,
                                       unsigned int timeout_sec, hu_run_result_t *out);

/**
 * True if `name` is an executable found on PATH, or an absolute/relative path with execute bit.
 * On Windows always returns false (not implemented).
 */
bool hu_exe_on_path(const char *name);

/** True if Ollama HTTP API responds on 127.0.0.1:11434 (best-effort; no network in HU_IS_TEST). */
bool hu_ollama_api_tags_reachable(void);

/** True if `python3 -c "import mlx_lm"` succeeds (macOS/Linux POSIX builds; false in HU_IS_TEST).
 */
bool hu_mlx_lm_module_available(void);

#endif /* HU_PROCESS_UTIL_H */
