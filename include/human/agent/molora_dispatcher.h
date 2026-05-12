#ifndef HU_AGENT_MOLORA_DISPATCHER_H
#define HU_AGENT_MOLORA_DISPATCHER_H

/* SOTA-2026 init-02 — MoLoRA per-channel persona routing dispatcher.
 *
 * Reads a per-turn `(provider, persona, channel, base_spec)` tuple and
 * pushes the matching expert mix into the provider via the existing
 * widened `hu_provider_load_adapter` surface. The dispatcher is the
 * only legitimate caller of `HU_LORA_APPLY_MODE_STACK` in-tree today —
 * every other call site uses REPLACE.
 *
 * The dispatcher deliberately does NOT own:
 *
 *   - the manifest of available channel experts (those come from the
 *     persona overlay; manifest/router files from the design doc §2.5
 *     are deferred to a follow-up sprint),
 *   - the W14 idle training job (`HU_JOB_MOLORA_ROUTER_TRAIN`),
 *   - the MLP router forward pass (`hu_lora_router_select`).
 *
 * What it does own:
 *
 *   1. Validate the provider and the persona/channel inputs.
 *   2. REPLACE the base adapter when one is provided (mirrors the W13
 *      daemon auto-load), forwarding NOT_SUPPORTED honestly so the
 *      caller falls through to base chat.
 *   3. STACK the channel expert from `hu_persona_overlay_t.lora_adapter_path`
 *      when present, honestly downgrading to "single-adapter only" if the
 *      provider returns NOT_SUPPORTED for STACK (huml/llamacpp).
 *
 * Cross-initiative contract:
 *
 *   - Cloud providers (`load_adapter == NULL`) return NOT_SUPPORTED for
 *     both REPLACE and STACK; the dispatcher propagates that verbatim so
 *     `m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` stays
 *     green.
 *   - Providers that support REPLACE but not STACK (huml, llamacpp) report
 *     the per-channel expert as "skipped" via `out->channel_expert_skipped`
 *     so the agent can log the degradation without crashing.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/lora.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — kept here to avoid pulling persona.h into every
 * dispatcher consumer (typing_simulator.h applies the same pattern).
 * Tests and call sites must include `human/persona.h` themselves. */
struct hu_persona;
typedef struct hu_persona hu_persona_t;

/* Diagnostics for the caller. Zero-init means "no work attempted".
 *
 * `base_loaded`:        REPLACE issued AND returned HU_OK.
 * `channel_stacked`:    STACK issued AND returned HU_OK on top of REPLACE.
 * `channel_expert_skipped`: an overlay-supplied channel expert existed but
 *                       STACK returned NOT_SUPPORTED. The base is still
 *                       active; only the channel-specific mix is gone.
 * `last_status`:        the most recent non-OK status the dispatcher saw,
 *                       useful for telemetry. HU_OK if every issued call
 *                       returned HU_OK.
 */
typedef struct hu_molora_apply_result {
    bool       base_loaded;
    bool       channel_stacked;
    bool       channel_expert_skipped;
    hu_error_t last_status;
} hu_molora_apply_result_t;

/* Apply the channel-aware MoLoRA selection to `provider`.
 *
 * Arguments:
 *   - `provider` (required): vtable-bearing provider; must be initialized.
 *   - `alloc`    (required): allocator forwarded to the provider helper.
 *   - `persona`  (optional): supplies channel-overlay routing. NULL ⇒
 *                            no overlay lookup; only the base path runs.
 *   - `channel`  (optional): channel name slice. NULL ⇒ no overlay
 *                            lookup even when `persona` is non-NULL.
 *   - `base_spec` (optional): the REPLACE base. NULL ⇒ skip REPLACE; STACK
 *                             will still NOT run because STACK without
 *                             a base is rejected by every provider.
 *   - `out` (optional): diagnostics. NULL ⇒ caller doesn't care.
 *
 * Return value:
 *   - HU_OK on success (with `out->channel_expert_skipped` set to true
 *     when STACK was unsupported — that is NOT an error).
 *   - HU_ERR_INVALID_ARGUMENT for NULL provider/alloc, or when the
 *     base_spec validates badly (forwarded from the provider helper).
 *   - HU_ERR_NOT_SUPPORTED when the provider has no `load_adapter` and
 *     a base_spec was supplied. Mirrors the daemon's cloud-fallthrough
 *     contract — caller falls back to base chat without an adapter.
 *   - Whatever the provider's `load_adapter(REPLACE)` returned, otherwise.
 *
 * The function never frees the spec or the persona; ownership belongs
 * to the caller. */
hu_error_t hu_molora_dispatcher_apply(hu_provider_t                 *provider,
                                      hu_allocator_t                *alloc,
                                      const hu_persona_t            *persona,
                                      const char                    *channel,
                                      size_t                         channel_len,
                                      const hu_lora_adapter_spec_t  *base_spec,
                                      hu_molora_apply_result_t      *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_MOLORA_DISPATCHER_H */
