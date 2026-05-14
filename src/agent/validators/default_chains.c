#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"

hu_error_t hu_validators_build_default_outbound_chain(hu_allocator_t *alloc,
                                                      const char *persona_name,
                                                      size_t persona_name_len,
                                                      hu_output_validator_chain_t **out) {
    (void)persona_name;
    (void)persona_name_len; /* used in P3 */
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_output_validator_chain_t *chain = NULL;
    hu_error_t err = hu_output_validator_chain_create(alloc, &chain);
    if (err != HU_OK)
        return err;

    hu_output_validator_t v;

#define ADD(creator_call)                              \
    do {                                               \
        err = (creator_call);                          \
        if (err != HU_OK)                              \
            goto fail;                                 \
        err = hu_output_validator_chain_add(chain, v); \
        if (err != HU_OK) {                            \
            hu_output_validator_deinit(&v, alloc);     \
            goto fail;                                 \
        }                                              \
    } while (0)

    ADD(hu_validator_response_guard_create(alloc, &v));
    ADD(hu_validator_channel_tags_create(alloc, &v));
    ADD(hu_validator_ai_phrases_create(alloc, &v));
    ADD(hu_validator_formal_structure_create(alloc, &v));

#undef ADD

    *out = chain;
    return HU_OK;

fail:
    hu_output_validator_chain_destroy(chain);
    return err;
}
