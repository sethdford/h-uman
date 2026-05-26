/* outbound/moderation.c — violence/hate/self-harm/PII gate.
 *
 * Phase A: stub returns SEND. Phase B implementation wraps the
 * existing local moderation primitive `hu_moderation_check`. On
 * flagged content:
 *
 *   - self_harm: leave intact (crisis-hotline path lives upstream)
 *   - violence/hate: REGENERATE with hint
 *     "Avoid endorsing harm. De-escalate. Acknowledge feelings."
 *   - PII (phone/SSN/CC): REJECT with reason "moderation_pii"
 *
 * Local-only; no offsite call. ~10ms latency.
 *
 * Corpus: no rows currently exercise this stage (the [SAFETY] leak
 * in #6 is caught by `shape` first because the safety block is huge).
 * Adversarial tests cover the contract.
 */

#include <stddef.h>

#include "human/agent/outbound_pipeline.h"

static hu_outbound_verdict_t moderation_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                            hu_outbound_context_t *ctx) {
    (void)self;
    (void)msg;
    (void)ctx;
    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_moderation = {
    .name = "moderation",
    .run = moderation_run,
    .state = NULL,
};
