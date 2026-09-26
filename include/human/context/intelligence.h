#ifndef HU_CONTEXT_INTELLIGENCE_H
#define HU_CONTEXT_INTELLIGENCE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* F68 — Protective Intelligence: boundaries per contact (avoid, redirect, lie) */
typedef struct hu_boundary {
    char *contact_id;
    size_t contact_id_len;
    char *topic;
    size_t topic_len;
    char *type;
    size_t type_len; /* "avoid", "redirect", "lie" */
    uint64_t set_at;
} hu_boundary_t;

bool hu_protective_topic_is_blocked(const hu_boundary_t *boundaries, size_t count,
                                    const char *topic, size_t topic_len);
hu_error_t hu_protective_build_prompt(hu_allocator_t *alloc, const hu_boundary_t *boundaries,
                                      size_t count, char **out, size_t *out_len);

/* F69 — Humor Generation */
typedef enum hu_humor_style {
    HU_HUMOR_STYLE_NONE = 0,
    HU_HUMOR_STYLE_CALLBACK,         /* reference to previous funny moment */
    HU_HUMOR_STYLE_OBSERVATIONAL,    /* noticing something funny about situation */
    HU_HUMOR_STYLE_SELF_DEPRECATING, /* making fun of self */
    HU_HUMOR_STYLE_ABSURD,           /* unexpected/surreal */
    HU_HUMOR_STYLE_DEADPAN           /* dry/understated */
} hu_humor_style_t;

typedef struct hu_humor_config {
    double humor_probability;   /* default 0.2 */
    bool never_during_crisis;   /* default true */
    bool never_during_serious;  /* default true */
    hu_humor_style_t preferred; /* default OBSERVATIONAL */
} hu_humor_config_t;

hu_humor_style_t hu_humor_select_style(double closeness, bool serious_topic, bool in_crisis,
                                       const hu_humor_config_t *config, uint32_t seed);
const char *hu_humor_style_str(hu_humor_style_t style);

/* F102 — Cognitive Load */
typedef struct hu_cognitive_state {
    uint32_t active_conversations; /* concurrent chats */
    uint32_t messages_this_hour;   /* volume */
    bool complex_topic_active;     /* deep discussion ongoing */
    double load_score;             /* 0.0-1.0 computed */
} hu_cognitive_state_t;

double hu_cognitive_compute_load(uint32_t active_convos, uint32_t msgs_this_hour,
                                 bool complex_topic);
hu_error_t hu_cognitive_build_directive(hu_allocator_t *alloc, const hu_cognitive_state_t *state,
                                        char **out, size_t *out_len);

#endif /* HU_CONTEXT_INTELLIGENCE_H */
