#ifndef SC_PROACTIVE_H
#define SC_PROACTIVE_H

#include "seaclaw/agent/commitment.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define SC_PROACTIVE_MAX_ACTIONS 32

typedef enum sc_proactive_action_type {
    SC_PROACTIVE_COMMITMENT_FOLLOW_UP,
    SC_PROACTIVE_MILESTONE,
    SC_PROACTIVE_CHECK_IN,
    SC_PROACTIVE_MORNING_BRIEFING,
    SC_PROACTIVE_PATTERN_INSIGHT,
} sc_proactive_action_type_t;

typedef struct sc_proactive_action {
    sc_proactive_action_type_t type;
    char *message;
    size_t message_len;
    double priority;
} sc_proactive_action_t;

typedef struct sc_proactive_result {
    sc_proactive_action_t actions[SC_PROACTIVE_MAX_ACTIONS];
    size_t count;
} sc_proactive_result_t;

sc_error_t sc_proactive_check(sc_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                               sc_proactive_result_t *out);
sc_error_t sc_proactive_check_extended(sc_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                                        const sc_commitment_t *commitments, size_t commitment_count,
                                        const char *const *pattern_subjects,
                                        const uint32_t *pattern_counts, size_t pattern_count,
                                        sc_proactive_result_t *out);
sc_error_t sc_proactive_build_context(const sc_proactive_result_t *result,
                                       sc_allocator_t *alloc, size_t max_actions,
                                       char **out, size_t *out_len);
void sc_proactive_result_deinit(sc_proactive_result_t *result, sc_allocator_t *alloc);

#endif /* SC_PROACTIVE_H */
