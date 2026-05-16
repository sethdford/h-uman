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
 * configured on the policy but no sandbox method actually took effect.
 *
 * Pure predicate — extracted so the deny-by-default contract is unit-testable
 * without forking a child. The fork path in shell.c calls into the same logic.
 *
 * @param policy           Security policy (may be NULL — "no sandbox configured" path).
 * @param apply_applied    True iff vtable->apply() returned HU_OK in this child.
 * @param wrap_attempted   True iff hu_sandbox_is_available() returned true and
 *                         hu_sandbox_wrap_command() was called.
 * @param wrap_succeeded   True iff wrap_command returned HU_OK with wrapped_count > 0.
 *
 * Returns true when the child MUST _exit() rather than exec /bin/sh:
 *   - sandbox configured but wrap attempted and failed, OR
 *   - sandbox configured but no method (apply or wrap) actually applied.
 * Returns false when exec may proceed (no sandbox configured, or one took effect).
 */
bool hu_shell_must_deny_unsandboxed(const hu_security_policy_t *policy, bool apply_applied,
                                    bool wrap_attempted, bool wrap_succeeded);

#endif /* HU_TOOLS_SHELL_H */
