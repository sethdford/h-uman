#ifndef HU_AGENT_GRAPH_GROUNDING_H
#define HU_AGENT_GRAPH_GROUNDING_H

#include "human/agent/memory_loader.h"
#include "human/core/error.h"
#include <stddef.h>

typedef enum hu_graph_grounding_mode {
    HU_GRAPH_GROUNDING_OFF = 0,
    HU_GRAPH_GROUNDING_SHADOW,
    HU_GRAPH_GROUNDING_ON,
} hu_graph_grounding_mode_t;

/* Reads HU_GRAPH_GROUNDING: unset/"off"/"0" -> OFF, "shadow" -> SHADOW,
 * "on"/"1" -> ON. Unknown values -> OFF (fail-safe). */
hu_graph_grounding_mode_t hu_graph_grounding_mode(void);

/* Best-effort: assembles markdown of the top community summaries for
 * `contact_id`. On any error/empty result, sets *out=NULL, *out_len=0 and
 * returns HU_OK. Caller frees *out via loader->alloc. `max_chars` caps output
 * (0 -> default 600). */
hu_error_t hu_graph_ground_load(hu_memory_loader_t *loader, const char *contact_id,
                                size_t contact_id_len, size_t max_chars, char **out,
                                size_t *out_len);

#endif /* HU_AGENT_GRAPH_GROUNDING_H */
