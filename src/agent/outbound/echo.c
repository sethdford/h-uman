/* outbound/echo.c — semantic directive-echo detection.
 *
 * Phase A: stub returns SEND. Phase B implementation detects when the
 * LLM echoed back its prompt instead of generating content. Two
 * algorithms:
 *
 *   1. Token-overlap (≥40% with prompt_used) → REGENERATE
 *      Catches corpus #5 "reference something specific you know..."
 *
 *   2. Standalone-noun match against directive vocabulary → REJECT
 *      Catches corpus #7 "shared history", #8 same, #9 "principle",
 *      #10 "under 10 words"
 *
 * Both are deterministic, no LLM call needed.
 */

#include <stddef.h>

#include "human/agent/outbound_pipeline.h"

static hu_outbound_verdict_t echo_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                      hu_outbound_context_t *ctx) {
    (void)self;
    (void)msg;
    (void)ctx;
    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_echo = {
    .name = "echo",
    .run = echo_run,
    .state = NULL,
};
