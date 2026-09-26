#ifndef HU_TOOLS_SHELL_INTERNAL_H
#define HU_TOOLS_SHELL_INTERNAL_H

#include <stdbool.h>
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

/*
 * Maximum inherited env vars considered for sanitization. Generous relative
 * to a normal process environment (tens of entries). Entries past the cap
 * are NOT examined: they reach the shell child untouched rather than the
 * child's environment being silently truncated. shell.c logs one WARN per
 * process (in the parent, before fork) when a live environment exceeds it.
 */
#define HU_SHELL_MAX_ENV_VARS 512

/*
 * Count the entries of a NULL-terminated "KEY=VALUE" array, examining at most
 * `cap` slots. Returns min(count, cap). Sets *truncated (if non-NULL) to true
 * when a non-NULL entry exists at index `cap`, i.e. the array is longer than
 * the caller is willing to look at. Pure.
 */
size_t hu_shell_count_env(char *const *env, size_t cap, bool *truncated);

/*
 * Pure core of shell.c's fork-child environment sanitizer, seamed so it can be
 * exercised on an arbitrary array instead of the live `environ`.
 *
 * Examines the first min(cap, HU_SHELL_MAX_ENV_VARS) entries of the
 * NULL-terminated `env`, runs them through hu_shell_build_child_env(), and
 * writes the entries that sanitization DROPPED (the blocklisted ones) into
 * `blocked_out`, in their original order, sharing string pointers with `env`.
 * Stops writing at `blocked_cap`. Sets *truncated exactly as
 * hu_shell_count_env() does. Returns the number of entries written.
 *
 * Does not mutate `env` and does not call unsetenv(); the caller applies the
 * decision (shell.c does so with unsetenv in the fork child).
 */
size_t hu_shell_collect_blocked_env(char *const *env, size_t cap, char **blocked_out,
                                    size_t blocked_cap, bool *truncated);

#endif /* HU_TOOLS_SHELL_INTERNAL_H */
