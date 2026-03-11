#ifndef HU_CONTEXT_AUTHENTIC_H
#define HU_CONTEXT_AUTHENTIC_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_authentic_behavior {
    HU_AUTH_NONE = 0,
    HU_AUTH_NARRATION,        /* F103: spontaneous life narration */
    HU_AUTH_EMBODIMENT,       /* F104: physical state ("I'm so tired") */
    HU_AUTH_IMPERFECTION,     /* F105: being wrong on purpose */
    HU_AUTH_COMPLAINING,      /* F106: mundane complaints */
    HU_AUTH_GOSSIP,           /* F107: social commentary */
    HU_AUTH_RANDOM_THOUGHT,   /* F108: random trains of thought */
    HU_AUTH_MEDIUM_AWARE,     /* F109: "texting you from bed" */
    HU_AUTH_RESISTANCE,       /* F110: "I don't feel like talking" */
    HU_AUTH_EXISTENTIAL,      /* F111: deep questions */
    HU_AUTH_CONTRADICTION,    /* F112: holding contradictory views */
    HU_AUTH_GUILT,            /* F113: "I should text mom" */
    HU_AUTH_LIFE_THREAD,      /* F114: running narrative */
    HU_AUTH_BAD_DAY           /* F115: recovery mode */
} hu_authentic_behavior_t;

typedef struct hu_authentic_config {
    double narration_probability;        /* default 0.10 */
    double embodiment_probability;       /* default 0.08 */
    double imperfection_probability;      /* default 0.05 */
    double complaining_probability;       /* default 0.07 */
    double gossip_probability;            /* default 0.04 */
    double random_thought_probability;    /* default 0.06 */
    double medium_awareness_probability;  /* default 0.05 */
    double resistance_probability;        /* default 0.03 */
    double existential_probability;       /* default 0.02 */
    double contradiction_probability;     /* default 0.03 */
    double guilt_probability;             /* default 0.04 */
    double life_thread_probability;       /* default 0.05 */
    bool bad_day_active;
    uint32_t bad_day_duration_hours;      /* default 8 */
} hu_authentic_config_t;

typedef struct hu_authentic_state {
    hu_authentic_behavior_t active;
    char *context;
    size_t context_len;
    double intensity; /* 0-1 */
} hu_authentic_state_t;

/* Select an authentic behavior based on probabilities and seed */
hu_authentic_behavior_t hu_authentic_select(const hu_authentic_config_t *config,
                                            double closeness, bool serious_topic,
                                            uint32_t seed);

/* Build prompt directive for the selected behavior */
hu_error_t hu_authentic_build_directive(hu_allocator_t *alloc,
                                        hu_authentic_behavior_t behavior,
                                        const char *life_context, size_t ctx_len,
                                        char **out, size_t *out_len);

/* F114: Life thread — persistent narrative */
hu_error_t hu_life_thread_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_life_thread_insert_sql(const char *thread, size_t thread_len,
                                     uint64_t timestamp, char *buf, size_t cap, size_t *out_len);
hu_error_t hu_life_thread_query_active_sql(char *buf, size_t cap, size_t *out_len);

/* F115: Bad day recovery */
bool hu_authentic_is_bad_day(bool bad_day_active, uint64_t bad_day_start,
                              uint64_t now_ms, uint32_t duration_hours);
hu_error_t hu_bad_day_build_directive(hu_allocator_t *alloc, char **out, size_t *out_len);

const char *hu_authentic_behavior_str(hu_authentic_behavior_t b);
void hu_authentic_state_deinit(hu_allocator_t *alloc, hu_authentic_state_t *s);

#endif /* HU_CONTEXT_AUTHENTIC_H */
