/* outbound/crosstalk.c — cross-contact bleed detection.
 *
 * Phase A: stub returns SEND. Phase B implementation:
 *
 *   - Compute char-5-gram set of msg->content
 *   - For each OTHER contact in memory (contact_id != recipient_contact_id),
 *     compute n-gram fingerprint over their last 7 days of inbound msgs
 *   - If Jaccard(msg, other_contact) > 0.4, REJECT with
 *     "crosstalk_<other_contact_id>"
 *
 * This is the STAGE that would have caught the Annie/Mindy/Betty
 * incident at egress regardless of the upstream feed-scope bug.
 *
 * Per Q-2 user decision: char-5-gram Jaccard, 7-day window,
 * ~10ms/send budget. Cached n-gram set per contact, invalidated on
 * new inbound.
 *
 * Corpus coverage: #1, #2, #3 (the lonely-bleed), #4 (timestamp
 * metadata leak — detected by metadata pattern, not n-gram).
 */

#include <stddef.h>

#include "human/agent/outbound_pipeline.h"

static hu_outbound_verdict_t crosstalk_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                           hu_outbound_context_t *ctx) {
    (void)self;
    (void)msg;
    (void)ctx;
    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_crosstalk = {
    .name = "crosstalk",
    .run = crosstalk_run,
    .state = NULL,
};
