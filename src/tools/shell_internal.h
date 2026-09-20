#ifndef HU_TOOLS_SHELL_INTERNAL_H
#define HU_TOOLS_SHELL_INTERNAL_H

#include <stddef.h>

/*
 * Build the sanitized child environment from an input "KEY=VALUE" array
 * (e.g. a snapshot of `environ`).
 *
 * Copies up to `out_cap` pointers from `in_env` into `out_env` (pointers are
 * shared with `in_env`, nothing is duplicated), then runs the result through
 * `hu_exec_env_sanitize()` to drop blocklisted names (LD_PRELOAD, MAVEN_OPTS,
 * GLIBC_TUNABLES, etc. — see include/human/security/exec_env.h). Returns the
 * number of surviving entries.
 *
 * Pure — does not fork, exec, read `environ`, or call unsetenv. Extracted
 * from shell.c's fork children so the sanitization step is unit-testable
 * without spawning a process (shell_execute's real fork/exec path is stubbed
 * out under HU_IS_TEST, same reasoning as hu_shell_must_deny_unsandboxed in
 * human/tools/shell.h).
 *
 * @param in_env   Input "KEY=VALUE" pointers. May be NULL if in_count is 0.
 * @param in_count Number of entries in in_env.
 * @param out_env  Output buffer for surviving pointers.
 * @param out_cap  Capacity of out_env.
 * @return Number of entries written to out_env (0 on invalid arguments).
 */
size_t hu_shell_build_child_env(char *const *in_env, size_t in_count, char **out_env,
                                size_t out_cap);

#endif /* HU_TOOLS_SHELL_INTERNAL_H */
