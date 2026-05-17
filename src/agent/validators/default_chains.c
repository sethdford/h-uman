#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/core/log.h"
#include "human/observability/validator_telemetry.h"
#include "human/observer.h"

#include <string.h>

hu_error_t hu_validators_build_default_outbound_chain(hu_allocator_t *alloc,
                                                      const char *persona_name,
                                                      size_t persona_name_len,
                                                      hu_output_validator_chain_t **out) {
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
    ADD(hu_validator_assistant_closer_create(alloc, &v));
    ADD(hu_validator_persona_narrator_create(alloc, persona_name, persona_name_len, &v));
    ADD(hu_validator_role_consistency_create(alloc, &v));
    ADD(hu_validator_persona_fidelity_create(alloc, &v));

#undef ADD

    *out = chain;
    return HU_OK;

fail:
    hu_output_validator_chain_destroy(chain);
    return err;
}

hu_chain_apply_outcome_t hu_validator_chain_apply_default_in_place(
    hu_allocator_t *alloc, hu_observer_t *observer, const char *persona_name,
    size_t persona_name_len, const char *log_tag, char *buf, size_t *len_inout, size_t cap) {
    if (!alloc || !buf || !len_inout || !log_tag || cap == 0)
        return HU_CHAIN_APPLY_SKIPPED;

    hu_output_validator_chain_t *chain = NULL;
    hu_error_t build_err =
        hu_validators_build_default_outbound_chain(alloc, persona_name, persona_name_len, &chain);
    if (build_err != HU_OK) {
        hu_log_error("validator_chain", observer,
                     "chain BUILD failed for %s (err=%s) -- keeping buffer (deny-by-default "
                     "responsibility lives with caller)",
                     log_tag, hu_error_string(build_err));
        return HU_CHAIN_APPLY_SKIPPED;
    }

    hu_validator_context_t vctx = {0};
    vctx.persona_name = persona_name;
    vctx.persona_name_len = persona_name_len;

    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    hu_error_t exec_err =
        hu_output_validator_chain_execute(chain, alloc, &vctx, buf, *len_inout, &cr);
    if (exec_err != HU_OK) {
        hu_log_error("validator_chain", observer,
                     "chain EXECUTE failed for %s (err=%s) -- keeping buffer (deny-by-default "
                     "responsibility lives with caller)",
                     log_tag, hu_error_string(exec_err));
        hu_chain_result_free(alloc, &cr);
        hu_output_validator_chain_destroy(chain);
        return HU_CHAIN_APPLY_SKIPPED;
    }

    hu_observer_emit_validator_decision(observer, &cr, &vctx, *len_inout);

    hu_chain_apply_outcome_t outcome = HU_CHAIN_APPLY_KEPT;
    if (cr.final_decision == HU_VALIDATOR_REJECT) {
        hu_log_warn("validator_chain", observer, "chain REJECT (via %s) -- suppressing %s",
                    cr.deciding_validator_name ? cr.deciding_validator_name : "unknown", log_tag);
        buf[0] = '\0';
        *len_inout = 0;
        outcome = HU_CHAIN_APPLY_SUPPRESSED;
    } else if (cr.final_text && cr.final_text_len > 0 &&
               (cr.final_text != buf || cr.final_text_len != *len_inout)) {
        /* Chain rewrote into an owned buffer. Copy into the caller's buffer,
         * truncating to cap-1 to preserve NUL terminator. */
        size_t copy_len = cr.final_text_len < cap ? cr.final_text_len : cap - 1;
        memcpy(buf, cr.final_text, copy_len);
        buf[copy_len] = '\0';
        *len_inout = copy_len;
        outcome = HU_CHAIN_APPLY_REWROTE;
    }
    /* else: PASS or rewrite-equal-to-input — buf unchanged. */

    hu_chain_result_free(alloc, &cr);
    hu_output_validator_chain_destroy(chain);
    return outcome;
}
