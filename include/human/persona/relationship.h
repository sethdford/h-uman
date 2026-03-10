#ifndef HU_RELATIONSHIP_H
#define HU_RELATIONSHIP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

typedef enum hu_relationship_stage {
    HU_REL_NEW,      /* 0-5 sessions */
    HU_REL_FAMILIAR, /* 5-20 sessions */
    HU_REL_TRUSTED,  /* 20-50 sessions */
    HU_REL_DEEP,     /* 50+ sessions */
} hu_relationship_stage_t;

typedef struct hu_relationship_state {
    hu_relationship_stage_t stage;
    uint32_t session_count;
    uint32_t total_turns;
} hu_relationship_state_t;

void hu_relationship_new_session(hu_relationship_state_t *state);
void hu_relationship_update(hu_relationship_state_t *state, uint32_t turn_count);
hu_error_t hu_relationship_build_prompt(hu_allocator_t *alloc,
                                         const hu_relationship_state_t *state,
                                         char **out, size_t *out_len);
hu_error_t hu_relationship_data_init(hu_allocator_t *alloc);
void hu_relationship_data_cleanup(hu_allocator_t *alloc);

#endif
