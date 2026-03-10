#ifndef HU_PERSONA_REPLAY_H
#define HU_PERSONA_REPLAY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/channel.h"
#include <stddef.h>
#include <stdint.h>

#define HU_REPLAY_MAX_INSIGHTS 10

typedef struct hu_replay_insight {
    char *observation;      /* "Response to 'how was your day' felt generic" */
    size_t observation_len;
    char *recommendation;   /* "Use more specific follow-up questions" */
    size_t recommendation_len;
    int score_delta;        /* positive = good, negative = needs improvement */
} hu_replay_insight_t;

typedef struct hu_replay_result {
    hu_replay_insight_t insights[HU_REPLAY_MAX_INSIGHTS];
    size_t insight_count;
    int overall_score;      /* 0-100 */
    char *summary;          /* "Conversation was mostly natural but 2 responses felt robotic" */
    size_t summary_len;
} hu_replay_result_t;

hu_error_t hu_replay_analyze(hu_allocator_t *alloc,
                             const hu_channel_history_entry_t *entries, size_t entry_count,
                             uint32_t max_chars, hu_replay_result_t *out);
void hu_replay_result_deinit(hu_replay_result_t *result, hu_allocator_t *alloc);

/* Build a context string from replay insights for future prompt injection.
 * Summarizes what worked and what didn't. Caller owns returned string. */
char *hu_replay_build_context(hu_allocator_t *alloc, const hu_replay_result_t *result,
                              size_t *out_len);

#endif
