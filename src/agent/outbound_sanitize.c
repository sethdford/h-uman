/* Outbound-message sanitization — compatibility shim over the
 * Sprint 59 outbound validation pipeline.
 *
 * The original (2026-05-26) implementation hardcoded directive-echo
 * blocklists and U+FFFC stripping inline. Sprint 59 replaces that
 * with a composable 6-stage pipeline (strip → shape → echo →
 * crosstalk → persona → moderation). This file is now a thin
 * adapter: callers using the old `hu_outbound_sanitize` API get
 * the upgraded pipeline behavior without any call-site changes.
 *
 * Pipeline → API mapping:
 *   SEND        → return true, content/len unchanged
 *   REWRITE     → return true, content/len updated in-place
 *   REGENERATE  → return false, reason set
 *   REJECT      → return false, reason set
 *
 * The caller buffer is reused for REWRITE: if the rewritten length
 * is <= original length, we memcpy back. (strip can only shrink, so
 * this always holds for the current stage set.)
 *
 * Path: HU_OUTBOUND_PATH_PROACTIVE — the most defensive config; runs
 * all six stages. Callers that need a lighter path will be migrated
 * to the pipeline directly in Phase D2.
 *
 * Crosstalk gracefully degrades when no lookup callback is wired
 * (logs once per process). The wrapper does not provide memory or
 * persona context; pure-egress checks (strip/shape/echo/persona/
 * moderation) still run.
 */

#include "human/agent/outbound_sanitize.h"

#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

bool hu_outbound_sanitize(char *content, size_t *content_len_inout, const char **reason_out) {
    if (!content || !content_len_inout) {
        if (reason_out)
            *reason_out = "null_input";
        return false;
    }
    if (*content_len_inout == 0) {
        if (reason_out)
            *reason_out = "empty_input";
        return false;
    }

    /* System allocator — pipeline needs one for stage scratch buffers
     * (n-gram sets, rewrite buffers, etc.). All buffers are freed
     * before return. */
    static hu_allocator_t alloc_storage;
    static int alloc_init = 0;
    if (!alloc_init) {
        alloc_storage = hu_system_allocator();
        alloc_init = 1;
    }
    hu_allocator_t *alloc = &alloc_storage;

    hu_outbound_pipeline_t *pipeline = NULL;
    if (hu_outbound_pipeline_for_path(alloc, HU_OUTBOUND_PATH_PROACTIVE, &pipeline) != HU_OK ||
        !pipeline) {
        /* Fail open — pipeline initialization shouldn't ever block a
         * legitimate send. */
        if (reason_out)
            *reason_out = NULL;
        return true;
    }

    /* The pipeline owns msg->content during the run (so it can
     * REWRITE by freeing+replacing the buffer). Copy the caller's
     * content into a heap buffer the pipeline can manage. */
    size_t orig_len = *content_len_inout;
    char *heap_content = (char *)alloc->alloc(alloc->ctx, orig_len + 1);
    if (!heap_content) {
        hu_outbound_pipeline_destroy(pipeline);
        if (reason_out)
            *reason_out = NULL;
        return true; /* fail open */
    }
    memcpy(heap_content, content, orig_len);
    heap_content[orig_len] = '\0';

    hu_outbound_message_t msg = {0};
    msg.content = heap_content;
    msg.content_len = orig_len;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 0; /* this API can't regenerate; treat as REJECT */

    hu_outbound_verdict_t verdict = {0};
    hu_error_t err = hu_outbound_pipeline_run(pipeline, &msg, &ctx, &verdict);

    bool ok = false;
    const char *reason = NULL;

    if (err != HU_OK) {
        /* Pipeline runtime error — fail open. */
        ok = true;
    } else {
        switch (verdict.kind) {
        case HU_OUTBOUND_SEND:
            /* Possibly REWRITE-applied; either way msg.content is
             * the final body. Copy back to caller buffer if it
             * fits. (Strip only ever shrinks, so this holds for
             * the current stage set.) */
            if (msg.content_len <= orig_len) {
                memcpy(content, msg.content, msg.content_len);
                if (msg.content_len < orig_len)
                    content[msg.content_len] = '\0';
                *content_len_inout = msg.content_len;
                ok = true;
            } else {
                /* Rewrite expanded — reject conservatively. */
                reason = "rewrite_expansion";
                ok = false;
            }
            break;
        case HU_OUTBOUND_REJECT:
            reason = verdict.reason ? verdict.reason : "rejected";
            ok = false;
            break;
        case HU_OUTBOUND_REGENERATE:
            /* No budget — treat as REJECT for this legacy API. */
            reason = verdict.reason ? verdict.reason : "regenerate_requested";
            ok = false;
            break;
        case HU_OUTBOUND_REWRITE:
            /* Unreachable — pipeline applies rewrites and returns SEND. */
            reason = "rewrite_uncaught";
            ok = false;
            break;
        }
    }

    /* Free msg.content (the pipeline may have replaced it via REWRITE). */
    if (msg.content)
        alloc->free(alloc->ctx, msg.content, msg.content_len + 1);
    hu_outbound_verdict_clear(&verdict, alloc);
    hu_outbound_pipeline_destroy(pipeline);

    if (reason_out)
        *reason_out = reason;
    return ok;
}
