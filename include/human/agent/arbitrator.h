#ifndef HU_AGENT_ARBITRATOR_H
#define HU_AGENT_ARBITRATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_DIRECTIVE_EMOTIONAL   (1u << 0)
#define HU_DIRECTIVE_BEHAVIORAL  (1u << 1)
#define HU_DIRECTIVE_MEMORY      (1u << 2)
#define HU_DIRECTIVE_PROACTIVE   (1u << 3)
#define HU_DIRECTIVE_SAFETY      (1u << 4)
#define HU_DIRECTIVE_IDENTITY    (1u << 5)

typedef struct hu_directive {
    char *content;
    size_t content_len;
    char *source; /* "energy_match", "emotional_checkin", "inside_joke", etc. */
    size_t source_len;
    double priority;   /* computed score 0.0–1.0 */
    size_t token_cost; /* estimated tokens (chars / 4 approximation) */
    uint32_t category; /* bitfield of HU_DIRECTIVE_* */
    bool required;     /* safety directives are never filtered out */
} hu_directive_t;

typedef struct hu_directive_conflict {
    const char *source_a;
    const char *source_b;
    const char *winner; /* which source wins, or NULL for higher-priority wins */
} hu_directive_conflict_t;

typedef struct hu_arbitration_config {
    size_t max_directive_tokens; /* default 1500 */
    size_t max_directives;       /* default 7 */
} hu_arbitration_config_t;

typedef struct hu_arbitration_result {
    hu_directive_t *selected; /* allocated array of selected directives (copies) */
    size_t selected_count;
    size_t total_tokens;     /* sum of token costs of selected directives */
    size_t suppressed_count; /* how many were filtered out */
} hu_arbitration_result_t;

/* Score a directive's priority based on context factors */
double hu_directive_compute_priority(uint32_t category, double recency,
                                     double emotional_weight,
                                     double relationship_depth,
                                     double conversation_phase, double novelty);

/* Resolve conflicts between directives. Removes losers in-place, freeing their
   content/source via alloc. Returns new count after conflict resolution. */
size_t hu_arbitrator_resolve_conflicts(hu_allocator_t *alloc,
                                       hu_directive_t *directives, size_t count,
                                       const hu_directive_conflict_t *conflict_table,
                                       size_t conflict_count);

/* Select top directives within token budget.
   Allocates result->selected. Caller must free via hu_arbitration_result_deinit. */
hu_error_t hu_arbitrator_select(hu_allocator_t *alloc,
                                const hu_directive_t *candidates,
                                size_t candidate_count,
                                const hu_arbitration_config_t *config,
                                hu_arbitration_result_t *result);

/* Estimate token count for a string (chars / 4, minimum 1) */
size_t hu_directive_estimate_tokens(const char *content, size_t content_len);

/* Free a directive's allocated strings */
void hu_directive_deinit(hu_allocator_t *alloc, hu_directive_t *d);

/* Free arbitration result */
void hu_arbitration_result_deinit(hu_allocator_t *alloc,
                                  hu_arbitration_result_t *result);

#endif
