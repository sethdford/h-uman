/* src/ml/molora.c — MoLoRA per-channel persona routing (Phase 1).
 *
 * Phase 1 uses a static channel-to-slot lookup table with FNV-1a hashing.
 * Known channels get a dedicated expert slot (1–6); unknown channels fall
 * back to the baseline slot 0.  The mixture always includes slot 0 at
 * >= macro_mode_floor weight, plus the channel expert. */

#include "human/ml/molora.h"
#include <string.h>

struct hu_molora_router {
    float macro_mode_floor;
};

/* FNV-1a 32-bit hash for channel name lookup. */
static uint32_t fnv1a(const char *s, size_t len) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Phase 1 channel-to-slot mapping.  Returns 0 (baseline) for unknown. */
static uint8_t channel_to_slot(const char *name, size_t len) {
    uint32_t h = fnv1a(name, len);
    /* Pre-computed FNV-1a hashes for tier-1 channels. */
    switch (h) {
    case 0xa98e2f16u: return 1; /* "telegram" */
    case 0x0f7e7a32u: return 2; /* "discord" */
    case 0x13e4aa54u: return 3; /* "imessage" */
    case 0x993107f2u: return 4; /* "slack" */
    case 0x0b87f4f6u: return 5; /* "cli" */
    case 0x99316b42u: return 6; /* "email" */
    default:          return 0;
    }
}

hu_error_t hu_molora_router_create(hu_allocator_t *alloc,
                                   const hu_molora_router_config_t *config,
                                   hu_molora_router_t **out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_molora_router_t *r = alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r) return HU_ERR_OUT_OF_MEMORY;

    float floor = config ? config->macro_mode_floor : 0.3f;
    if (floor < 0.0f) floor = 0.0f;
    if (floor > 0.8f) floor = 0.8f;
    r->macro_mode_floor = floor;

    *out = r;
    return HU_OK;
}

hu_error_t hu_molora_router_route(const hu_molora_router_t *router,
                                  const char *channel_name,
                                  size_t channel_name_len,
                                  uint8_t message_class,
                                  uint8_t macro_mode,
                                  hu_molora_mixture_t *out) {
    (void)message_class;
    (void)macro_mode;

    if (!out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    float floor = router ? router->macro_mode_floor : 0.3f;

    uint8_t slot = 0;
    if (channel_name && channel_name_len > 0)
        slot = channel_to_slot(channel_name, channel_name_len);

    if (slot == 0) {
        out->slots[0]   = 0;
        out->weights[0] = 1.0f;
        out->n          = 1;
    } else {
        out->slots[0]   = 0;
        out->weights[0] = floor;
        out->slots[1]   = slot;
        out->weights[1] = 1.0f - floor;
        out->n          = 2;
    }
    return HU_OK;
}

void hu_molora_router_deinit(hu_molora_router_t *router,
                             hu_allocator_t *alloc) {
    if (router && alloc)
        alloc->free(alloc->ctx, router, sizeof(*router));
}
