/* include/human/ml/molora.h -- MoLoRA per-channel persona routing.
 *
 * Mixture-of-LoRA-Experts routing for per-channel persona adaptation.
 * Maps (channel, message_class, macro_mode) to a sparse mixture of
 * LoRA adapter slots. Phase 1 uses a channel-to-slot lookup table;
 * a learned MLP router replaces it once labelled turn data accumulates.
 *
 * Slot 0 is reserved for the persona macro-mode baseline adapter
 * (always present in the mixture at >= macro_mode_floor weight).
 * Slots 1..6 are channel experts. Slot 7 is reserved for
 * verifier-driven TTT.
 */
#ifndef HU_ML_MOLORA_H
#define HU_ML_MOLORA_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_MOLORA_MAX_SLOTS  8
#define HU_MOLORA_MAX_ACTIVE 3

/* Maps a logical slot id to an on-disk adapter path. */
typedef struct hu_molora_slot {
    uint8_t     id;
    const char *adapter_path;
    size_t      adapter_path_len;
} hu_molora_slot_t;

/* Sparse mixture: up to MAX_ACTIVE (slot, weight) pairs.
 * Weights sum to 1.0 when n > 0. */
typedef struct hu_molora_mixture {
    uint8_t slots[HU_MOLORA_MAX_ACTIVE];
    float   weights[HU_MOLORA_MAX_ACTIVE];
    size_t  n;
} hu_molora_mixture_t;

/* Router configuration. Pass NULL to hu_molora_router_create for
 * defaults (macro_mode_floor = 0.3). */
typedef struct hu_molora_router_config {
    float macro_mode_floor; /* slot-0 weight floor; clamped [0.0, 0.8] */
} hu_molora_router_config_t;

typedef struct hu_molora_router hu_molora_router_t;

/* Create a router from config (or defaults when config is NULL).
 * Phase 1: channel-to-slot lookup table with FNV-1a hashing.
 * Returns HU_ERR_INVALID_ARGUMENT on NULL alloc or NULL out. */
hu_error_t hu_molora_router_create(hu_allocator_t *alloc,
                                   const hu_molora_router_config_t *config,
                                   hu_molora_router_t **out);

/* Compute a sparse mixture for the given turn context.
 * channel_name: channel identifier (e.g. "telegram", "imessage").
 * message_class: heuristic tag [0..7] (ack, chitchat, question, ...).
 * macro_mode: persona macro-mode [0..7] (default, playful, ...).
 *
 * Always succeeds when router and out are non-NULL. On NULL router
 * returns a degenerate mixture (slot 0 at weight 1.0). */
hu_error_t hu_molora_router_route(const hu_molora_router_t *router,
                                  const char *channel_name,
                                  size_t channel_name_len,
                                  uint8_t message_class,
                                  uint8_t macro_mode,
                                  hu_molora_mixture_t *out);

/* Free all router resources. Safe to call with NULL router. */
void hu_molora_router_deinit(hu_molora_router_t *router,
                             hu_allocator_t *alloc);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_MOLORA_H */
