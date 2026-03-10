#ifndef HU_INFO_ASYMMETRY_H
#define HU_INFO_ASYMMETRY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_info_gap_type {
    HU_GAP_AGENT_KNOWS,   /* agent knows, contact doesn't */
    HU_GAP_CONTACT_KNOWS, /* contact knows, agent doesn't yet */
    HU_GAP_SHARED,        /* both know */
    HU_GAP_NEITHER,       /* gap in both */
} hu_info_gap_type_t;

typedef struct hu_info_gap {
    char *topic;
    size_t topic_len;
    hu_info_gap_type_t type;
    bool disclosure_appropriate; /* safe to share naturally */
} hu_info_gap_t;

#define HU_INFO_GAP_MAX 32

typedef struct hu_info_asymmetry {
    hu_info_gap_t gaps[HU_INFO_GAP_MAX];
    size_t gap_count;
} hu_info_asymmetry_t;

hu_error_t hu_info_asymmetry_analyze(hu_info_asymmetry_t *result, hu_allocator_t *alloc,
                                     const char *agent_context, size_t agent_context_len,
                                     const char *contact_context, size_t contact_context_len);

hu_error_t hu_info_asymmetry_build_guidance(const hu_info_asymmetry_t *asym, hu_allocator_t *alloc,
                                            char **out, size_t *out_len);

void hu_info_asymmetry_deinit(hu_info_asymmetry_t *asym, hu_allocator_t *alloc);

#endif
