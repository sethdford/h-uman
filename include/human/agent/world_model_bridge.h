#ifndef HU_AGENT_WORLD_MODEL_BRIDGE_H
#define HU_AGENT_WORLD_MODEL_BRIDGE_H

/* W9 wire bridge (FIX 12).
 *
 * The W7 memory facade and the legacy `hu_memory_t` from `human/memory.h`
 * use the same struct tag (`struct hu_memory`), so any translation unit that
 * already pulls in legacy `human/memory.h` (e.g. anything that includes
 * `human/agent.h`) cannot also include `human/memory/memory.h` or
 * `human/agent/world_model.h` -- the C compiler sees a redefinition.
 *
 * This bridge gives `agent_turn.c` and `daemon.c` a way to use the W7 facade
 * + `hu_world_model_load` without paying that include cost. The bridge owns
 * its own translation unit (`world_model_bridge.c`) where ONLY the W7
 * headers are pulled in; everyone else talks to the bridge through the
 * unique opaque tag `struct hu_w7_facade`.
 *
 * Same pattern as `agent->verifier_graph` (FIX 2): isolate the type
 * collision behind a fresh forward declaration. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque W7 facade handle. Define lives in world_model_bridge.c. */
struct hu_w7_facade;
typedef struct hu_w7_facade hu_w7_facade_t;

/* Open a W7 facade backed by `graph` (the v1 backends). Caller owns the
 * returned pointer and must close with hu_w7_facade_close. */
hu_error_t hu_w7_facade_open(hu_graph_t *graph, hu_allocator_t *alloc, hu_w7_facade_t **out);

void hu_w7_facade_close(hu_w7_facade_t *facade, hu_allocator_t *alloc);

/* Render the cached world model for `contact_id` into a prompt-ready text
 * block. Returns HU_OK with `*out_text == NULL`, `*out_len == 0` when there
 * is no information worth surfacing -- callers should treat that as "no
 * world model context available" and skip injection.
 *
 * The text format mirrors the persona/personal_model sections in the system
 * prompt (FIX 1): a labeled markdown block with subsections for goals,
 * negatives, theory-of-mind, and recent topics. Caller owns the returned
 * pointer and must free with `alloc->free`.
 *
 * `now_ms == 0` means "use OS clock". */
hu_error_t hu_w7_render_world_model(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    int64_t now_ms, char **out_text, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_WORLD_MODEL_BRIDGE_H */
