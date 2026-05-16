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

void hu_chain_result_free(hu_allocator_t *alloc, hu_chain_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->final_text && result->final_text_owned) {
        alloc->free(alloc->ctx, (void *)result->final_text, result->final_text_len + 1);
    }
    if (result->reject_reason && result->reject_reason_owned) {
        alloc->free(alloc->ctx, (void *)result->reject_reason, result->reject_reason_len + 1);
    }
    memset(result, 0, sizeof(*result));
    result->deciding_validator = (size_t)-1;
}

hu_error_t hu_output_validator_chain_execute(const hu_output_validator_chain_t *chain,
                                             hu_allocator_t *alloc,
                                             const hu_validator_context_t *vctx,
                                             const char *response, size_t response_len,
                                             hu_chain_result_t *out) {
    if (!chain || !alloc || !response || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->deciding_validator = (size_t)-1;

    const char *current = response;
    size_t current_len = response_len;
    bool current_owned = false;

    for (size_t i = 0; i < chain->len; i++) {
        const hu_output_validator_t *v = &chain->entries[i];
        hu_validator_result_t r;
        memset(&r, 0, sizeof(r));
        hu_error_t err = v->vtable->validate(v->ctx, alloc, vctx, current, current_len, &r);
        if (err != HU_OK) {
            if (current_owned) {
                alloc->free(alloc->ctx, (void *)current, current_len + 1);
            }
            hu_validator_result_free(alloc, &r);
            return err;
        }
        if (r.decision == HU_VALIDATOR_PASS) {
            hu_validator_result_free(alloc, &r);
            continue;
        }
        if (r.decision == HU_VALIDATOR_REWRITE) {
            /* Free the previous buffer if we owned it. */
            if (current_owned) {
                alloc->free(alloc->ctx, (void *)current, current_len + 1);
            }
            current = r.text;
            current_len = r.text_len;
            current_owned = r.text_owned;
            /* Free only the reason (if owned); text ownership transferred. */
            if (r.reason && r.reason_owned) {
                alloc->free(alloc->ctx, (void *)r.reason, r.reason_len + 1);
            }
            out->rewrite_count++;
            out->deciding_validator = i;
            out->deciding_validator_name = v->vtable->name(v->ctx);
            continue;
        }
        /* REJECT — short circuit. Free the intermediate rewrite buffer if any. */
        if (current_owned) {
            alloc->free(alloc->ctx, (void *)current, current_len + 1);
        }
        out->final_decision = HU_VALIDATOR_REJECT;
        out->final_text = NULL;
        out->final_text_len = 0;
        out->final_text_owned = false;
        out->deciding_validator = i;
        out->deciding_validator_name = v->vtable->name(v->ctx);
        out->reject_reason = r.reason;
        out->reject_reason_len = r.reason_len;
        out->reject_reason_owned = r.reason_owned;
        out->reject_count = 1;
        /* Don't call hu_validator_result_free on r — we transferred reason ownership. */
        return HU_OK;
    }

    out->final_decision = current_owned ? HU_VALIDATOR_REWRITE : HU_VALIDATOR_PASS;
    out->final_text = current;
    out->final_text_len = current_len;
    out->final_text_owned = current_owned;
    return HU_OK;
}
