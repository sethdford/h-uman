#ifndef HU_AGENT_BURST_EGRESS_H
#define HU_AGENT_BURST_EGRESS_H

/* Sprint 60 — outbound-safety wiring for burst sub-sends.
 *
 * The daemon's burst code at src/daemon.c near the burst-fragment
 * for-loop parses a single LLM response into N fragments (3-4
 * typical) and sends each via ch->channel->vtable->send. Before
 * this helper existed, those sends BYPASSED the outbound pipeline
 * entirely — the crosstalk / persona / shape stages never ran on
 * any burst fragment.
 *
 * That meant: if the LLM hallucinated a cross-contact bleed in any
 * single fragment, the daemon shipped it. The Sprint 59 Phase B
 * crosstalk wiring + the Sprint 60 SQLite lookup didn't protect this
 * path because the pipeline simply never ran on it.
 *
 * hu_burst_egress_validate_fragment closes that gap. The daemon's
 * burst loop calls this helper for each fragment before
 * channel->send; on REJECT, the loop breaks and drops all remaining
 * fragments (so the user doesn't see partial garbled bursts).
 *
 * Pipeline path: HU_OUTBOUND_PATH_REACTIVE (strip + crosstalk).
 * Reactive is the right semantic because a burst is the reply to an
 * inbound message — the same path response_guard already runs.
 *
 * Lifecycle: the helper owns the heap-copy of input through the
 * pipeline. On SEND, *out_content is the heap-owned buffer the
 * caller MUST free via alloc->free(ctx, *out_content, *out_len + 1).
 * On REJECT, the helper frees its copy and sets *out_content = NULL.
 *
 * Note on the int *out_kind boundary: this header intentionally does
 * NOT include outbound_pipeline.h to keep the public API decoupled
 * from a name-collision between `struct hu_outbound_stage` (pipeline
 * subsystem) and `enum hu_outbound_stage` (channel.h delivery phase).
 * The integer values match hu_outbound_verdict_kind_t exactly:
 *
 *     0 = SEND       — fragment is safe; caller channel->sends
 *                      *out_content / *out_len
 *     1 = REWRITE    — folded into SEND by the helper; caller
 *                      receives kind=SEND with rewritten content
 *     2 = REGENERATE — no retry budget in the burst path; caller
 *                      treats as REJECT (drops remaining fragments)
 *     3 = REJECT     — content violated a stage; caller drops and
 *                      breaks the burst loop
 *
 * These match the values defined in include/human/agent/outbound_pipeline.h.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boundary constants matching hu_outbound_verdict_kind_t — exposed
 * here so callers don't have to include outbound_pipeline.h just to
 * branch on the verdict. Values are stable wire-level integers. */
#define HU_BURST_EGRESS_SEND 0
#define HU_BURST_EGRESS_REWRITE 1
#define HU_BURST_EGRESS_REGENERATE 2
#define HU_BURST_EGRESS_REJECT 3

/* Validate a single burst fragment through the outbound pipeline.
 *
 * Inputs:
 *   alloc                       — allocator used for heap-copy and
 *                                 any pipeline rewrite buffers
 *   channel_name                — channel id ("imessage", "slack",
 *                                 etc.) for shape/persona stages;
 *                                 NULL falls back to iMessage rules
 *   recipient_contact_id, _len  — for crosstalk's "this contact vs
 *                                 OTHER contacts" lookup
 *   input, input_len            — the fragment text (read-only;
 *                                 helper makes its own heap copy)
 *
 * Outputs:
 *   *out_content / *out_len     — on SEND, heap-owned final content
 *                                 (may be REWRITE-modified by strip
 *                                 stage). Caller MUST free.
 *                                 On REJECT/REGENERATE, NULL/0.
 *   *out_kind                   — one of the HU_BURST_EGRESS_*
 *                                 constants above. The daemon's
 *                                 burst loop branches:
 *                                   SEND  → channel->send + free
 *                                   else  → break (drop remaining)
 *
 * Returns:
 *   HU_OK                       — verdict + content populated
 *   HU_ERR_INVALID_ARGUMENT     — any required arg is NULL
 *   HU_ERR_OUT_OF_MEMORY        — heap copy alloc failed
 *   propagated pipeline errors  — pipeline_for_path / _run failed
 */
hu_error_t hu_burst_egress_validate_fragment(hu_allocator_t *alloc, const char *channel_name,
                                             const char *recipient_contact_id,
                                             size_t recipient_contact_id_len, const char *input,
                                             size_t input_len, char **out_content, size_t *out_len,
                                             int *out_kind);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_BURST_EGRESS_H */
