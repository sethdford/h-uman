#ifndef HU_THEORY_OF_MIND_H
#define HU_THEORY_OF_MIND_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_TOM_MAX_BELIEFS      64
#define HU_TOM_MAX_EXPECTATIONS 32

typedef enum hu_belief_type {
    HU_BELIEF_KNOWS,    /* contact knows this fact */
    HU_BELIEF_ASSUMES,  /* contact likely assumes this */
    HU_BELIEF_UNAWARE,  /* contact doesn't know this yet */
    HU_BELIEF_MISTAKEN, /* contact has wrong info about this */
} hu_belief_type_t;

/* Direction of belief modeling */
typedef enum hu_tom_belief_direction {
    HU_TOM_AI_ABOUT_USER, /* what the AI believes about the user (existing) */
    HU_TOM_USER_ABOUT_AI, /* what the user expects the AI to know */
} hu_tom_belief_direction_t;

/* What the user expects the AI to know */
typedef enum hu_tom_expected_knowledge {
    HU_TOM_EXPECT_REMEMBERS,   /* user expects AI remembers a fact */
    HU_TOM_EXPECT_UNDERSTANDS, /* user expects AI understands a concept */
    HU_TOM_EXPECT_TRACKS,      /* user expects AI tracks an ongoing topic */
} hu_tom_expected_knowledge_t;

/* A user expectation about what the AI should know */
typedef struct hu_tom_expectation {
    char *topic;
    size_t topic_len;
    hu_tom_expected_knowledge_t knowledge_type;
    int64_t recorded_at;
} hu_tom_expectation_t;

/* A gap where user expects knowledge the AI doesn't have */
typedef struct hu_tom_gap {
    char *topic;
    size_t topic_len;
    hu_tom_expected_knowledge_t knowledge_type;
} hu_tom_gap_t;

typedef struct hu_tom_belief {
    char *topic;
    size_t topic_len;
    hu_belief_type_t type;
    float confidence; /* 0.0-1.0 */
    int64_t last_updated;
} hu_tom_belief_t;

typedef struct hu_tom_belief_state {
    char *contact_id;
    size_t contact_id_len;
    hu_tom_belief_t beliefs[HU_TOM_MAX_BELIEFS];
    size_t belief_count;
    hu_tom_expectation_t expectations[HU_TOM_MAX_EXPECTATIONS];
    size_t expectation_count;
} hu_tom_belief_state_t;

/* Initialize a belief state for a contact */
hu_error_t hu_tom_init(hu_tom_belief_state_t *state, hu_allocator_t *alloc, const char *contact_id,
                       size_t contact_id_len);

/* Record a belief from conversation evidence */
hu_error_t hu_tom_record_belief(hu_tom_belief_state_t *state, hu_allocator_t *alloc, const char *topic,
                                size_t topic_len, hu_belief_type_t type, float confidence);

/* Build context string summarizing what the contact knows/doesn't know */
hu_error_t hu_tom_build_context(const hu_tom_belief_state_t *state, hu_allocator_t *alloc, char **out,
                                size_t *out_len);

/* Record what the user expects the AI to know about a topic */
hu_error_t hu_tom_record_user_expectation(hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                          const char *topic, size_t topic_len,
                                          hu_tom_expected_knowledge_t knowledge_type);

/* Detect gaps: topics where user expects knowledge AI doesn't have.
 * Returns array of gaps (caller owns). gap_count set to number found. */
hu_error_t hu_tom_detect_gaps(const hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                              hu_tom_gap_t **gaps_out, size_t *gap_count);

/* Build directive string for detected gaps, for prompt injection.
 * Returns NULL if no gaps. Caller owns returned string. */
char *hu_tom_build_gap_directive(hu_allocator_t *alloc, const hu_tom_gap_t *gaps, size_t gap_count,
                                 size_t *out_len);

/* Free a gap array returned by hu_tom_detect_gaps */
void hu_tom_gaps_free(hu_allocator_t *alloc, hu_tom_gap_t *gaps, size_t count);

/* Detect if message text indicates user expects AI to know something.
 * Returns true if a pattern like "remember when", "you know my" is found.
 * If true, topic_out and topic_len_out are set to the extracted topic. */
bool hu_tom_detect_user_expectation(const char *text, size_t text_len, const char **topic_out,
                                    size_t *topic_len_out,
                                    hu_tom_expected_knowledge_t *knowledge_type_out);

/* Free all beliefs in a state */
void hu_tom_deinit(hu_tom_belief_state_t *state, hu_allocator_t *alloc);

#endif /* HU_THEORY_OF_MIND_H */
