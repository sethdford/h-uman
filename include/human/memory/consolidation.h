#ifndef HU_MEMORY_CONSOLIDATION_H
#define HU_MEMORY_CONSOLIDATION_H

#include "human/memory.h"
#include "human/provider.h"

typedef struct hu_consolidation_config {
    uint32_t decay_days;
    double decay_factor;
    uint32_t dedup_threshold; /* 0-100 token overlap percentage */
    uint32_t max_entries;
    hu_provider_t *provider; /* optional; NULL = skip connection discovery */
    const char *model;       /* model name for LLM calls; NULL uses provider default */
    size_t model_len;
} hu_consolidation_config_t;

#define HU_CONSOLIDATION_DEFAULTS \
    {.decay_days = 30,            \
     .decay_factor = 0.9,         \
     .dedup_threshold = 85,       \
     .max_entries = 10000,        \
     .provider = NULL,            \
     .model = NULL,               \
     .model_len = 0}

uint32_t hu_similarity_score(const char *a, size_t a_len, const char *b, size_t b_len);
hu_error_t hu_memory_consolidate(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const hu_consolidation_config_t *config);

#endif
