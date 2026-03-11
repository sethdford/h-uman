#ifndef HU_INTELLIGENCE_REFLECTION_H
#define HU_INTELLIGENCE_REFLECTION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_feedback_signal_type {
    HU_FEEDBACK_POSITIVE = 0, /* they responded warmly */
    HU_FEEDBACK_NEGATIVE,      /* left on read, short response */
    HU_FEEDBACK_NEUTRAL,
    HU_FEEDBACK_CORRECTION     /* "that's not what I meant" */
} hu_feedback_signal_type_t;

typedef struct hu_feedback_signal {
    char *contact_id;
    size_t contact_id_len;
    hu_feedback_signal_type_t type;
    char *context;
    size_t context_len; /* what triggered it */
    char *our_action;
    size_t our_action_len; /* what we did */
    uint64_t timestamp;
} hu_feedback_signal_t;

typedef struct hu_reflection_entry {
    int64_t id;
    char *period;
    size_t period_len; /* "daily", "weekly" */
    char *insights;
    size_t insights_len; /* JSON array of insights */
    char *improvements;
    size_t improvements_len;
    uint64_t created_at;
} hu_reflection_entry_t;

typedef struct hu_skill_observation {
    char *skill_name;
    size_t skill_name_len;
    char *contact_id;
    size_t contact_id_len;
    double proficiency; /* 0-1 */
    uint32_t practice_count;
    uint64_t last_practiced;
} hu_skill_observation_t;

hu_error_t hu_reflection_create_tables_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_feedback_insert_sql(const hu_feedback_signal_t *fb, char *buf, size_t cap,
                                 size_t *out_len);
hu_error_t hu_feedback_query_recent_sql(const char *contact_id, size_t len, uint32_t limit,
                                        char *buf, size_t cap, size_t *out_len);
hu_error_t hu_reflection_insert_sql(const hu_reflection_entry_t *entry, char *buf, size_t cap,
                                   size_t *out_len);
hu_error_t hu_reflection_query_latest_sql(const char *period, size_t period_len, char *buf,
                                          size_t cap, size_t *out_len);

/* F78: Detect feedback signals from response patterns */
hu_feedback_signal_type_t hu_feedback_classify(uint32_t response_time_seconds,
                                               size_t response_length, bool contains_question,
                                               bool left_on_read);

/* F80: Score relationship skill proficiency */
double hu_skill_proficiency_score(uint32_t positive_count, uint32_t total_count,
                                 uint32_t practice_count);

/* F81: Transfer learning across contacts */
double hu_cross_contact_learning_weight(double source_proficiency, double relevance);

hu_error_t hu_reflection_build_prompt(hu_allocator_t *alloc,
                                     const hu_reflection_entry_t *latest,
                                     const hu_feedback_signal_t *recent_feedback, size_t fb_count,
                                     char **out, size_t *out_len);

const char *hu_feedback_type_str(hu_feedback_signal_type_t type);
void hu_feedback_signal_deinit(hu_allocator_t *alloc, hu_feedback_signal_t *fb);
void hu_reflection_entry_deinit(hu_allocator_t *alloc, hu_reflection_entry_t *entry);
void hu_skill_observation_deinit(hu_allocator_t *alloc, hu_skill_observation_t *obs);

#endif /* HU_INTELLIGENCE_REFLECTION_H */
