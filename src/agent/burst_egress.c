/* src/agent/burst_egress.c
 *
 * Sprint 60 — outbound-safety wiring for burst sub-sends. See header
 * for the contract; see daemon.c near the burst-fragment for-loop
 * for the production call site.
 *
 * Implementation notes:
 *
 *   - The pipeline runner's REWRITE handling calls alloc->free on
 *     msg->content before substituting the replacement. So
 *     msg->content MUST be heap-owned by `alloc` before the run —
 *     we cannot pass a stack pointer or the caller's read-only
 *     buffer. The helper allocates its own copy.
 *
 *   - On SEND or REWRITE, msg->content holds the final content.
 *     Either way, ownership transfers to the caller via *out_content.
 *
 *   - On REJECT or REGENERATE, the helper frees its copy. Caller
 *     gets out_content=NULL, out_len=0, kind = REJECT/REGENERATE.
 *
 *   - REGENERATE has no retry budget in the burst path. The daemon
 *     treats REGENERATE the same as REJECT: break the loop, log,
 *     drop remaining fragments.
 *
 *   - The header exposes int *out_kind (not hu_outbound_verdict_kind_t *)
 *     to keep the public API decoupled from outbound_pipeline.h's
 *     name-collision with channel.h. The .c uses the enum internally.
 */

#include "human/agent/burst_egress.h"
#include "human/agent/outbound_pipeline.h"
#include "human/core/error.h"

#include <stdlib.h>
#include <string.h>

hu_error_t hu_burst_egress_validate_fragment(hu_allocator_t *alloc, const char *channel_name,
                                             const char *recipient_contact_id,
                                             size_t recipient_contact_id_len, const char *input,
                                             size_t input_len, char **out_content, size_t *out_len,
                                             int *out_kind) {
    if (!alloc || !recipient_contact_id || !input || !out_content || !out_len || !out_kind)
        return HU_ERR_INVALID_ARGUMENT;
    *out_content = NULL;
    *out_len = 0;
    *out_kind = HU_BURST_EGRESS_REJECT;

    /* Heap-copy the input so the pipeline can apply REWRITE in place. */
    char *content = (char *)alloc->alloc(alloc->ctx, input_len + 1);
    if (!content)
        return HU_ERR_OUT_OF_MEMORY;
    if (input_len > 0)
        memcpy(content, input, input_len);
    content[input_len] = '\0';

    hu_outbound_pipeline_t *pipeline = NULL;
    hu_error_t err = hu_outbound_pipeline_for_path(alloc, HU_OUTBOUND_PATH_REACTIVE, &pipeline);
    if (err != HU_OK || !pipeline) {
        alloc->free(alloc->ctx, content, input_len + 1);
        return err != HU_OK ? err : HU_ERR_INTERNAL;
    }

    hu_outbound_message_t msg = {0};
    msg.content = content;
    msg.content_len = input_len;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = alloc;
    ctx.path = HU_OUTBOUND_PATH_REACTIVE;
    ctx.recipient_contact_id = recipient_contact_id;
    ctx.recipient_contact_id_len = recipient_contact_id_len;
    ctx.channel_name = channel_name;
    /* No regenerate budget in the burst path. */
    ctx.regenerate_budget = 0;

    hu_outbound_verdict_t verdict = {0};
    err = hu_outbound_pipeline_run(pipeline, &msg, &ctx, &verdict);

    if (err == HU_OK &&
        (verdict.kind == HU_OUTBOUND_SEND || verdict.kind == HU_OUTBOUND_REWRITE)) {
        *out_content = msg.content;
        *out_len = msg.content_len;
        /* Normalize REWRITE -> SEND so the caller has one happy-path branch. */
        *out_kind = HU_BURST_EGRESS_SEND;
    } else {
        if (msg.content)
            alloc->free(alloc->ctx, msg.content, msg.content_len + 1);
        if (err == HU_OK) {
            switch (verdict.kind) {
            case HU_OUTBOUND_REGENERATE:
                *out_kind = HU_BURST_EGRESS_REGENERATE;
                break;
            case HU_OUTBOUND_REJECT:
            default:
                *out_kind = HU_BURST_EGRESS_REJECT;
                break;
            }
        } else {
            *out_kind = HU_BURST_EGRESS_REJECT;
        }
    }

    hu_outbound_verdict_clear(&verdict, alloc);
    hu_outbound_pipeline_destroy(pipeline);
    return err;
}
