#ifndef HU_TOOLS_SHELL_H
#define HU_TOOLS_SHELL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_shell_create(hu_allocator_t *alloc, const char *workspace_dir,
                           size_t workspace_dir_len, hu_security_policy_t *policy, hu_tool_t *out);

/**
 * Decide whether the shell child must refuse to exec because a sandbox was
 * configured on the policy but no sandbox method actually took effect on
 * this process.
 *
 * Pure predicate — extracted so the deny-by-default contract is unit-testable
 * without forking a child. The fork path in shell.c calls into the same logic.
 *
 * @param policy         Security policy (may be NULL — "no sandbox configured" path).
 * @param apply_applied  True iff vtable->apply() returned HU_OK in this child.
 *                       Kernel-level Landlock / seccomp lockdown is active.
 * @param wrap_succeeded True iff hu_sandbox_wrap_command() returned HU_OK with
 *                       wrapped_count > 0 and the wrapped argv was launched.
 *
 * Returns true when the child MUST _exit() rather than exec /bin/sh:
 *   - sandbox configured on the policy, AND
 *   - neither apply_applied nor wrap_succeeded — i.e. the operator asked for
 *     containment and the process is unsandboxed.
 * Returns false when exec may proceed (no sandbox configured, or at least
 * one sandbox method took effect on this process).
 *
 * NOTE: A prior signature included a `wrap_attempted` parameter that combined
 * with `!wrap_succeeded` to force a deny — but that short-circuited the
 * apply_applied check, so a process with active kernel sandbox AND a failed
 * wrap call was denied despite being contained. Removed (cursor-bot review on
 * PR #90).
 */
bool hu_shell_must_deny_unsandboxed(const hu_security_policy_t *policy, bool apply_applied,
                                    bool wrap_succeeded);

#endif /* HU_TOOLS_SHELL_H */
