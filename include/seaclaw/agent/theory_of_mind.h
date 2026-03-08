#ifndef SC_THEORY_OF_MIND_H
#define SC_THEORY_OF_MIND_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_TOM_MAX_BELIEFS 64

typedef enum sc_belief_type {
    SC_BELIEF_KNOWS,    /* contact knows this fact */
    SC_BELIEF_ASSUMES,  /* contact likely assumes this */
    SC_BELIEF_UNAWARE,  /* contact doesn't know this yet */
    SC_BELIEF_MISTAKEN, /* contact has wrong info about this */
} sc_belief_type_t;

typedef struct sc_belief {
    char *topic;
    size_t topic_len;
    sc_belief_type_t type;
    float confidence; /* 0.0-1.0 */
    int64_t last_updated;
} sc_belief_t;

typedef struct sc_belief_state {
    char *contact_id;
    size_t contact_id_len;
    sc_belief_t beliefs[SC_TOM_MAX_BELIEFS];
    size_t belief_count;
} sc_belief_state_t;

/* Initialize a belief state for a contact */
sc_error_t sc_tom_init(sc_belief_state_t *state, sc_allocator_t *alloc, const char *contact_id,
                       size_t contact_id_len);

/* Record a belief from conversation evidence */
sc_error_t sc_tom_record_belief(sc_belief_state_t *state, sc_allocator_t *alloc, const char *topic,
                                size_t topic_len, sc_belief_type_t type, float confidence);

/* Build context string summarizing what the contact knows/doesn't know */
sc_error_t sc_tom_build_context(const sc_belief_state_t *state, sc_allocator_t *alloc, char **out,
                                size_t *out_len);

/* Free all beliefs in a state */
void sc_tom_deinit(sc_belief_state_t *state, sc_allocator_t *alloc);

#endif /* SC_THEORY_OF_MIND_H */
