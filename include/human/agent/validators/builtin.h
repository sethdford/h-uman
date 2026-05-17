/* Factory functions for the built-in output validators. Each returns
 * a fully-constructed hu_output_validator_t by value via the out pointer;
 * the chain takes ownership (its deinit will be called when the chain
 * is destroyed). */
#ifndef HU_AGENT_VALIDATORS_BUILTIN_H
#define HU_AGENT_VALIDATORS_BUILTIN_H

#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/observer.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_validator_response_guard_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_channel_tags_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_ai_phrases_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_formal_structure_create(hu_allocator_t *alloc, hu_output_validator_t *out);
hu_error_t hu_validator_cot_audit_create(hu_allocator_t *alloc, hu_output_validator_t *out);

/* P3 validators — Jordan-channel leak prevention (2026-05-14).
 *
 * F1: Detects prose-CoT narrating about the persona in third person.
 *     Pass persona_name/len (may be NULL/0 to always PASS — requires name to
 *     detect condition b). */
hu_error_t hu_validator_persona_narrator_create(hu_allocator_t *alloc, const char *persona_name,
                                                size_t persona_name_len,
                                                hu_output_validator_t *out);

/* F2: Strips known AI-helper closing phrases (REWRITE or PASS, never REJECT). */
hu_error_t hu_validator_assistant_closer_create(hu_allocator_t *alloc, hu_output_validator_t *out);

/* F3: Detects turn-boundary mid-message (first "\n\n" followed by bot speech). */
hu_error_t hu_validator_role_consistency_create(hu_allocator_t *alloc, hu_output_validator_t *out);

/* Persona-fidelity classifier — STUB for the M3 LoRA workstream. Currently
 * always PASSes. When M3 ships an on-device fidelity model this validator
 * will score responses against the persona's example bank and REJECT below
 * threshold. Wired into the chain now so the composition does not change
 * when M3 lands. */
hu_error_t hu_validator_persona_fidelity_create(hu_allocator_t *alloc, hu_output_validator_t *out);

/* persona_voice — REJECTs responses containing hard AI-identity disclosure
 * (e.g. "I'm a language model", "I don't have feelings") that the stripping
 * validators upstream cannot repair. Wired AFTER the strippers so prefixable
 * tells like "As an AI, " get cleaned in place; this validator only fires
 * on disclosure that is the substance of the message. See
 * src/agent/validators/persona_voice_validator.c for the doctrine note. */
hu_error_t hu_validator_persona_voice_create(hu_allocator_t *alloc, hu_output_validator_t *out);

/* Pure predicate factored out of the validator per
 * .claude/rules/security-predicate-extraction.md. Returns true when the
 * response is free of AI-identity disclosure as judged by
 * hu_conversation_check_ai_disclosure (the single source of truth for the
 * pattern list). Tests pin the truth table by calling this directly without
 * constructing an output-validator context. */
bool hu_persona_voice_response_is_clean(const char *response, size_t response_len);

/* Build the default outbound chain in registration order:
 *   1. response_guard          (REWRITE or REJECT special-tokens/thinking/degen/bullet-CoT)
 *   2. channel_tags            (REWRITE stripping)
 *   3. ai_phrases              (REWRITE stripping)
 *   4. formal_structure        (REWRITE stripping)
 *   5. assistant_closer        (REWRITE stripping — F2)
 *   6. persona_narrator        (REJECT on third-person narration — F1)
 *   7. role_consistency        (REJECT on mid-message role collapse — F3)
 *   8. persona_voice           (REJECT on hard AI-identity disclosure — 2026-05-17)
 *   9. persona_fidelity        (STUB; REJECT on low M3 fidelity score when wired)
 *
 * Note: cot_audit_validator is NOT wired in this default chain — it
 * operates on `reasoning_content`, not on the main reply content.
 *
 * persona_voice runs AFTER all strippers so prefixable tells like
 * "As an AI, " get cleaned in place before this validator sees them; it
 * only fires when disclosure is the substance of the response. */
hu_error_t hu_validators_build_default_outbound_chain(hu_allocator_t *alloc,
                                                      const char *persona_name,
                                                      size_t persona_name_len,
                                                      hu_output_validator_chain_t **out);

/* Outcome of hu_validator_chain_apply_default_in_place. */
typedef enum {
    HU_CHAIN_APPLY_KEPT,       /* PASS or rewrite-equal-to-input — buf unchanged. */
    HU_CHAIN_APPLY_REWROTE,    /* Chain rewrote — buf overwritten in place,
                                  *len_inout updated, NUL-terminated. Truncated
                                  to cap-1 if rewrite was oversized. */
    HU_CHAIN_APPLY_SUPPRESSED, /* REJECT — buf[0]='\0', *len_inout=0. Logged at
                                  WARN with the supplied log_tag. */
    HU_CHAIN_APPLY_SKIPPED,    /* Chain machinery failed (build or execute) —
                                  buf unchanged. Logged at ERROR. Callers that
                                  need deny-by-default should treat this the
                                  same as SUPPRESSED; callers with a defensive
                                  fallback (e.g. a strip pass) can keep the buf. */
} hu_chain_apply_outcome_t;

/* Build the default outbound chain, run it against (buf, *len_inout), and
 * apply the result IN PLACE. Consolidates the six-step "build → execute →
 * branch on decision → free → destroy" dance that was duplicated across
 * src/daemon.c (×5), src/daemon_cron.c (×1), and other in-place callers.
 *
 * Args:
 *   alloc, observer       — chain build + telemetry. observer may be NULL.
 *   persona_name/len      — passed into hu_validator_context_t. May be NULL/0.
 *   log_tag               — human-readable tag for log messages (e.g.
 *                           "scheduled send", "cron send"). Required, non-NULL.
 *   buf                   — in/out buffer.
 *   len_inout             — current length, updated in place.
 *   cap                   — buffer capacity in bytes (must be > 0; rewrites
 *                           are truncated to cap-1 to preserve the NUL).
 *
 * Never returns an error code — failures result in HU_CHAIN_APPLY_SKIPPED. */
hu_chain_apply_outcome_t hu_validator_chain_apply_default_in_place(
    hu_allocator_t *alloc, hu_observer_t *observer, const char *persona_name,
    size_t persona_name_len, const char *log_tag, char *buf, size_t *len_inout, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_VALIDATORS_BUILTIN_H */
