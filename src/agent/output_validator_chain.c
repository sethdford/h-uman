#include "human/agent/output_validator_chain.h"
#include "human/agent/output_validator.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <string.h>

#define HU_VALIDATOR_CHAIN_INITIAL_CAP 8

struct hu_output_validator_chain {
    hu_allocator_t *alloc;
    hu_output_validator_t *entries;
    size_t len;
    size_t cap;
};

hu_error_t hu_output_validator_chain_create(hu_allocator_t *alloc,
                                            hu_output_validator_chain_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_output_validator_chain_t *c =
        (hu_output_validator_chain_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->entries = (hu_output_validator_t *)alloc->alloc(
        alloc->ctx, HU_VALIDATOR_CHAIN_INITIAL_CAP * sizeof(hu_output_validator_t));
    if (!c->entries) {
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    c->len = 0;
    c->cap = HU_VALIDATOR_CHAIN_INITIAL_CAP;
    *out = c;
    return HU_OK;
}

void hu_output_validator_chain_destroy(hu_output_validator_chain_t *chain) {
    if (!chain)
        return;
    for (size_t i = 0; i < chain->len; i++) {
        hu_output_validator_deinit(&chain->entries[i], chain->alloc);
    }
    chain->alloc->free(chain->alloc->ctx, chain->entries,
                       chain->cap * sizeof(hu_output_validator_t));
    chain->alloc->free(chain->alloc->ctx, chain, sizeof(*chain));
}

hu_error_t hu_output_validator_chain_add(hu_output_validator_chain_t *chain,
                                         hu_output_validator_t v) {
    if (!chain || !v.vtable || !v.vtable->validate || !v.vtable->name)
        return HU_ERR_INVALID_ARGUMENT;
    if (chain->len == chain->cap) {
        size_t new_cap = chain->cap * 2;
        hu_output_validator_t *grow = (hu_output_validator_t *)chain->alloc->alloc(
            chain->alloc->ctx, new_cap * sizeof(hu_output_validator_t));
        if (!grow)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(grow, chain->entries, chain->len * sizeof(hu_output_validator_t));
        chain->alloc->free(chain->alloc->ctx, chain->entries,
                           chain->cap * sizeof(hu_output_validator_t));
        chain->entries = grow;
        chain->cap = new_cap;
    }
    chain->entries[chain->len++] = v;
    return HU_OK;
}

size_t hu_output_validator_chain_len(const hu_output_validator_chain_t *chain) {
    return chain ? chain->len : 0;
}
