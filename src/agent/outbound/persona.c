/* outbound/persona.c — Seth-voice fidelity check.
 *
 * Phase A: stub returns SEND. Phase B implementation:
 *
 *   - Reuse eval_shape_classifier from src/persona/shape_classifier.c
 *     to score msg->content against ctx->persona
 *   - If fidelity < 0.5, REGENERATE with hint
 *     "Sound more like Seth — casual, brief, no project jargon"
 *
 * Per Q-3 user decision: REGENERATE, not REJECT. False-positives in
 * persona detection would block legitimate brief messages
 * ("how are you" scores low on style features).
 *
 * Corpus coverage: #6 (the [SAFETY] block — doesn't sound like Seth),
 * #11-16 (Replay MCP project jargon to family — doesn't match Seth's
 * channel overlay for family contacts).
 */

#include <stddef.h>

#include "human/agent/outbound_pipeline.h"

static hu_outbound_verdict_t persona_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                         hu_outbound_context_t *ctx) {
    (void)self;
    (void)msg;
    (void)ctx;
    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_persona = {
    .name = "persona",
    .run = persona_run,
    .state = NULL,
};
