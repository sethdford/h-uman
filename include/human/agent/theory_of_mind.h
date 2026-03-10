#ifndef HU_THEORY_OF_MIND_H
#define HU_THEORY_OF_MIND_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_TOM_MAX_BELIEFS 64

typedef enum hu_belief_type {
    HU_BELIEF_KNOWS,    /* contact knows this fact */
    HU_BELIEF_ASSUMES,  /* contact likely assumes this */
    HU_BELIEF_UNAWARE,  /* contact doesn't know this yet */
    HU_BELIEF_MISTAKEN, /* contact has wrong info about this */
} hu_belief_type_t;

typedef struct hu_belief {
    char *topic;
    size_t topic_len;
    hu_belief_type_t type;
    float confidence; /* 0.0-1.0 */
    int64_t last_updated;
} hu_belief_t;

typedef struct hu_belief_state {
    char *contact_id;
    size_t contact_id_len;
    hu_belief_t beliefs[HU_TOM_MAX_BELIEFS];
    size_t belief_count;
} hu_belief_state_t;

/* Initialize a belief state for a contact */
hu_error_t hu_tom_init(hu_belief_state_t *state, hu_allocator_t *alloc, const char *contact_id,
                       size_t contact_id_len);

/* Record a belief from conversation evidence */
hu_error_t hu_tom_record_belief(hu_belief_state_t *state, hu_allocator_t *alloc, const char *topic,
                                size_t topic_len, hu_belief_type_t type, float confidence);

/* Build context string summarizing what the contact knows/doesn't know */
hu_error_t hu_tom_build_context(const hu_belief_state_t *state, hu_allocator_t *alloc, char **out,
                                size_t *out_len);

/* Free all beliefs in a state */
void hu_tom_deinit(hu_belief_state_t *state, hu_allocator_t *alloc);

#endif /* HU_THEORY_OF_MIND_H */
