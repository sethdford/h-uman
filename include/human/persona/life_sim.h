#ifndef HU_PERSONA_LIFE_SIM_H
#define HU_PERSONA_LIFE_SIM_H

#include "human/core/allocator.h"
#include <stddef.h>
#include <stdint.h>

/* hu_routine_block_t and hu_daily_routine_t are defined in persona.h.
 * Include persona.h so we can use them. */
#include "human/persona.h"

typedef struct hu_life_sim_state {
    const char *activity;
    const char *availability; /* "available" | "brief" | "slow" | "unavailable" */
    const char *mood_modifier;
    float availability_factor; /* 0.5=available, 1.0=brief, 2.0=slow, 5.0=unavailable */
} hu_life_sim_state_t;

/* Get current simulated state based on time. Uses routine_variance for ±15% jitter. */
hu_life_sim_state_t hu_life_sim_get_current(const hu_daily_routine_t *routine, int64_t now_ts,
                                            int day_of_week, uint32_t seed);

/* Build context string for prompt. "[LIFE CONTEXT: You just finished dinner...]" */
char *hu_life_sim_build_context(hu_allocator_t *alloc, const hu_life_sim_state_t *state,
                                 size_t *out_len);

#endif /* HU_PERSONA_LIFE_SIM_H */
