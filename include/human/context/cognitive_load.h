#ifndef HU_COGNITIVE_LOAD_H
#define HU_COGNITIVE_LOAD_H

#include <stdint.h>
#include <time.h>

typedef struct {
    float capacity;
    int conversation_depth;
    int hour_of_day;
    int day_of_week;
    const char *physical_state;
} hu_cognitive_state_t;

typedef struct {
    int peak_hour_start;
    int peak_hour_end;
    int low_hour_start;
    int low_hour_end;
    int fatigue_threshold;
    float monday_penalty;
    float friday_bonus;
} hu_cognitive_config_t;

hu_cognitive_state_t hu_cognitive_load_calculate(
    const hu_cognitive_config_t *config,
    int conversation_depth,
    time_t now);

const char *hu_cognitive_load_prompt_hint(const hu_cognitive_state_t *state);
int hu_cognitive_load_max_response_length(const hu_cognitive_state_t *state);

#endif
