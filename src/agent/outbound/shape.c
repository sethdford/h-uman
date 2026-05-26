/* outbound/shape.c — length + sentence-structure validation stage.
 *
 * Phase A: stub returns SEND. Phase B implementation rejects:
 *   - content > 200 chars (no Seth-shaped message is that long)
 *   - content with 60+ chars AND multiple sentence terminators
 *     (the F25 cross-contact bleed pattern in corpus #1, #2, #3, #6)
 *
 * Returns REGENERATE with hint "Reply must be under 80 chars, single
 * phrase" when shape is wrong — the LLM can usually fix this on
 * retry.
 */

#include <stddef.h>

#include "human/agent/outbound_pipeline.h"

static hu_outbound_verdict_t shape_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                       hu_outbound_context_t *ctx) {
    (void)self;
    (void)msg;
    (void)ctx;
    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_shape = {
    .name = "shape",
    .run = shape_run,
    .state = NULL,
};
