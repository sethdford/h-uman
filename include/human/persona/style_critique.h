#ifndef HU_PERSONA_STYLE_CRITIQUE_H
#define HU_PERSONA_STYLE_CRITIQUE_H

/* US-7.9 — Constitutional style self-critique at generation time.
 *
 * Pure literal pattern matcher (no regex, no LLM call).  Given the
 * final draft and the persona's style_rules[], reports whether any
 * rule fires.  Orchestration (calling the provider a second time to
 * regenerate) lives at the call site in src/agent/agent_turn.c.
 *
 * Rule parsing recognises two shapes:
 *   - "never start with '<X>'"  → PREFIX on <X>, word-boundary anchored
 *   - everything else            → SUBSTRING on a curated alias
 *                                  (em-dash, emoji, exclamation marks,
 *                                  …) or, if no alias matches, on the
 *                                  rule's last quoted span; falling
 *                                  back to the rule text itself.
 *
 * Comparisons are ASCII case-insensitive.  Multi-byte literals (e.g.
 * U+2014 em-dash, multi-codepoint emoji) are matched byte-for-byte.
 */

#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations to avoid pulling in heavy headers. */
struct hu_provider;
struct hu_allocator;
struct hu_observer;

/* Orchestration entry point used by the agent-turn hook AND by unit
 * tests that drive the regen path with a mock provider.
 *
 * Contract:
 *   - If style_rules_count == 0 OR no rule fires on the input draft,
 *     *out_response == NULL and *out_response_len == 0;
 *     *invocations_added == 0 (no regen attempted).
 *   - If a rule fires, exactly ONE regen attempt is made via
 *     provider->vtable->chat_with_system with an augmented system
 *     prompt; the regen output is written to *out_response /
 *     *out_response_len (caller owns and must free).  The unresolved
 *     event is emitted iff the regen output ALSO violates a rule.
 *   - Returns HU_OK in both cases (no violation, or violation with
 *     attempted regen); returns HU_ERR_INVALID_ARGUMENT on NULL
 *     required pointers.
 *
 * The original draft is NOT freed by this function; the caller owns
 * it and decides whether to swap to the regen result. */
hu_error_t hu_style_critique_run(struct hu_allocator *alloc, struct hu_provider *provider,
                                 struct hu_observer *observer, const char *system_prompt,
                                 size_t system_prompt_len, const char *user_message,
                                 size_t user_message_len, const char *model_name,
                                 size_t model_name_len, const char *draft, size_t draft_len,
                                 char *const *style_rules, size_t style_rules_count,
                                 char **out_response, size_t *out_response_len);

/* Returns HU_OK whether or not a rule fires.
 *   - On success with violated_rule_out NULL  → no rule fired.
 *   - On success with violated_rule_out set   → that rule fired
 *     (borrowed pointer into style_rules[i]; do not free).
 *
 * Returns HU_ERR_INVALID_ARGUMENT if draft is NULL or
 * violated_rule_out / violated_rule_len_out is NULL.
 *
 * style_rules may be NULL with style_rules_count == 0; treated as
 * "no rules, no violations". */
hu_error_t hu_style_critique_check(const char *draft, size_t draft_len, char *const *style_rules,
                                   size_t style_rules_count, const char **violated_rule_out,
                                   size_t *violated_rule_len_out);

#ifdef HU_IS_TEST
/* Test-only counters and reset shim.  Reset between tests. */
extern int hu_style_critique_test_unresolved_count;
extern int hu_style_critique_test_check_invocations;
void hu_style_critique_test_reset(void);

/* Increment the unresolved-violation counter from the call site (used
 * by the agent_turn.c hook when a second attempt also violates). */
void hu_style_critique_test_note_unresolved(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_STYLE_CRITIQUE_H */
