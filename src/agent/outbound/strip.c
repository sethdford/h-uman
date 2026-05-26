/* outbound/strip.c — character-normalization stage.
 *
 * Phase A: stub returns SEND. Phase B implementation strips:
 *   - U+FFFC OBJECT REPLACEMENT CHARACTER (iMessage attachment placeholder)
 *   - U+202E RIGHT-TO-LEFT OVERRIDE
 *   - U+200D ZERO WIDTH JOINER
 *   - U+200B ZERO WIDTH SPACE
 *
 * Returns REWRITE with stripped content if any chars were removed,
 * SEND otherwise.
 */

#include "human/agent/outbound_pipeline.h"

static hu_outbound_verdict_t strip_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                       hu_outbound_context_t *ctx) {
    (void)self;
    (void)msg;
    (void)ctx;
    /* Phase A stub. Phase B: walk UTF-8, strip codepoint set. */
    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_strip = {
    .name = "strip",
    .run = strip_run,
    .state = NULL,
};
