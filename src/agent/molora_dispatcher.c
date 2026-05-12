/* SOTA-2026 init-02 — MoLoRA per-channel persona routing dispatcher.
 *
 * Sits between the agent turn (or daemon auto-load) and the provider's
 * widened `load_adapter` vtable. The function is intentionally small —
 * one REPLACE call for the macro-mode base, one STACK call for the
 * overlay-selected channel expert, no allocations of its own. The
 * router MLP, manifest parser, and W14 idle-training job are deferred
 * to a later sprint per the brief.
 *
 * See `include/human/agent/molora_dispatcher.h` for the contract.
 */

#include "human/agent/molora_dispatcher.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/lora.h"
#include "human/persona.h"
#include "human/provider.h"

#include <stddef.h>
#include <string.h>

/* Treat NOT_SUPPORTED as a non-fatal "cloud provider, skip adapters"
 * signal — the daemon cloud-fallthrough pattern that
 * `m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` pins. */
static bool is_not_supported(hu_error_t e) { return e == HU_ERR_NOT_SUPPORTED; }

hu_error_t hu_molora_dispatcher_apply(hu_provider_t                 *provider,
                                      hu_allocator_t                *alloc,
                                      const hu_persona_t            *persona,
                                      const char                    *channel,
                                      size_t                         channel_len,
                                      const hu_lora_adapter_spec_t  *base_spec,
                                      hu_molora_apply_result_t      *out) {
    hu_molora_apply_result_t local = {0};
    if (out) {
        memset(out, 0, sizeof(*out));
        out->last_status = HU_OK;
    }
    if (!provider || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    /* Phase 1 — REPLACE base. Skipping the REPLACE entirely is legitimate
     * when the caller wants "channel-only" semantics, but every in-tree
     * caller today supplies a base. */
    if (base_spec) {
        hu_error_t base_err = hu_provider_load_adapter(provider, base_spec,
                                                       HU_LORA_APPLY_MODE_REPLACE);
        local.last_status = base_err;
        if (is_not_supported(base_err)) {
            /* Cloud-safety contract: propagate NOT_SUPPORTED verbatim so
             * the daemon falls back to base chat. Do NOT attempt STACK —
             * the provider has no adapter machinery at all. */
            if (out)
                *out = local;
            return HU_ERR_NOT_SUPPORTED;
        }
        if (base_err != HU_OK) {
            /* Real REPLACE failure (e.g. OOM, path traversal). Surface it;
             * STACK without a base is incoherent. */
            if (out)
                *out = local;
            return base_err;
        }
        local.base_loaded = true;
    }

    /* Phase 2 — overlay-driven STACK. Optional in every direction. */
    if (persona && channel && channel_len > 0) {
        const hu_persona_overlay_t *ov =
            hu_persona_find_overlay(persona, channel, channel_len);
        if (ov && ov->lora_adapter_path) {
            /* Derive the adapter id from the overlay; fall back to the
             * channel name so the active-pool label is always meaningful
             * for observability. */
            const char *id = ov->lora_adapter_id ? ov->lora_adapter_id : channel;
            size_t id_len = id ? strlen(id) : 0u;
            size_t path_len = strlen(ov->lora_adapter_path);
            if (id_len == 0 || path_len == 0) {
                /* Malformed overlay (empty path / empty fallback id).
                 * The persona loader's parse_overlay rejects empty
                 * `lora_adapter_path` on JSON load, but a programmatic
                 * caller might still build this in memory. */
                if (out)
                    *out = local;
                return HU_OK;
            }
            const hu_lora_adapter_spec_t stack_spec = {
                .path = ov->lora_adapter_path,
                .path_len = path_len,
                .id = id,
                .id_len = id_len,
                .alloc = alloc,
            };
            hu_error_t stack_err = hu_provider_load_adapter(provider, &stack_spec,
                                                            HU_LORA_APPLY_MODE_STACK);
            if (stack_err == HU_OK) {
                local.channel_stacked = true;
            } else if (is_not_supported(stack_err)) {
                /* Provider supports REPLACE but not STACK (huml, llamacpp
                 * today). The base is still active; the agent will run
                 * without the channel expert. Telemetry path only. */
                local.channel_expert_skipped = true;
                local.last_status = stack_err;
            } else {
                /* Real STACK failure — propagate so the caller knows the
                 * mix is incomplete and can decide whether to bail out. */
                local.last_status = stack_err;
                if (out)
                    *out = local;
                return stack_err;
            }
        }
    }

    if (out)
        *out = local;
    return HU_OK;
}
