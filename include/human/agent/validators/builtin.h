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

/* Build the default outbound chain in registration order:
 *   1. response_guard          (REWRITE or REJECT special-tokens/thinking/degen/bullet-CoT)
 *   2. channel_tags            (REWRITE stripping)
 *   3. ai_phrases              (REWRITE stripping)
 *   4. formal_structure        (REWRITE stripping)
 *
 * Note: cot_audit_validator is NOT wired in this default chain — it
 * operates on `reasoning_content`, not on the main reply content. The
 * P3 validators (assistant_closer, persona_narrator, role_consistency)
 * will be appended here in a follow-on task. */
hu_error_t hu_validators_build_default_outbound_chain(hu_allocator_t *alloc,
                                                      const char *persona_name,
                                                      size_t persona_name_len,
                                                      hu_output_validator_chain_t **out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_VALIDATORS_BUILTIN_H */
