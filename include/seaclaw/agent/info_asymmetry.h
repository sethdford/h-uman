#ifndef SC_INFO_ASYMMETRY_H
#define SC_INFO_ASYMMETRY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum sc_info_gap_type {
    SC_GAP_AGENT_KNOWS,   /* agent knows, contact doesn't */
    SC_GAP_CONTACT_KNOWS, /* contact knows, agent doesn't yet */
    SC_GAP_SHARED,        /* both know */
    SC_GAP_NEITHER,       /* gap in both */
} sc_info_gap_type_t;

typedef struct sc_info_gap {
    char *topic;
    size_t topic_len;
    sc_info_gap_type_t type;
    bool disclosure_appropriate; /* safe to share naturally */
} sc_info_gap_t;

#define SC_INFO_GAP_MAX 32

typedef struct sc_info_asymmetry {
    sc_info_gap_t gaps[SC_INFO_GAP_MAX];
    size_t gap_count;
} sc_info_asymmetry_t;

sc_error_t sc_info_asymmetry_analyze(sc_info_asymmetry_t *result, sc_allocator_t *alloc,
                                     const char *agent_context, size_t agent_context_len,
                                     const char *contact_context, size_t contact_context_len);

sc_error_t sc_info_asymmetry_build_guidance(const sc_info_asymmetry_t *asym, sc_allocator_t *alloc,
                                            char **out, size_t *out_len);

void sc_info_asymmetry_deinit(sc_info_asymmetry_t *asym, sc_allocator_t *alloc);

#endif
