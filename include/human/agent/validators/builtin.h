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

/* Build the default outbound chain in registration order:
 *   1. response_guard          (REWRITE or REJECT special-tokens/thinking/degen/bullet-CoT)
 *   2. channel_tags            (REWRITE stripping)
 *   3. ai_phrases              (REWRITE stripping)
 *   4. formal_structure        (REWRITE stripping)
 *   5. assistant_closer        (REWRITE stripping — F2)
 *   6. persona_narrator        (REJECT on third-person narration — F1)
 *   7. role_consistency        (REJECT on mid-message role collapse — F3)
 *
 * Note: cot_audit_validator is NOT wired in this default chain — it
 * operates on `reasoning_content`, not on the main reply content. */
hu_error_t hu_validators_build_default_outbound_chain(hu_allocator_t *alloc,
                                                      const char *persona_name,
                                                      size_t persona_name_len,
                                                      hu_output_validator_chain_t **out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_VALIDATORS_BUILTIN_H */
