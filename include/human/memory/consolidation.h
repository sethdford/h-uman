#ifndef HU_MEMORY_CONSOLIDATION_H
#define HU_MEMORY_CONSOLIDATION_H

#include "human/memory.h"
#include "human/provider.h"
#include <stdbool.h>

typedef struct hu_consolidation_config {
    uint32_t decay_days;
    double decay_factor;
    uint32_t dedup_threshold; /* 0-100 token overlap percentage */
    uint32_t max_entries;
    hu_provider_t *provider; /* unread since connection discovery was removed (2026-10-01) */
    const char *model;       /* unread, as above */
    size_t model_len;
} hu_consolidation_config_t;

/* decay_days / dedup_threshold match the config_merge.c defaults for
 * behavior.decay_days / behavior.dedup_threshold, so a NULL config and a
 * freshly loaded one consolidate identically. Production callers go through
 * hu_agent_consolidation_config(), which overrides both from config. */
#define HU_CONSOLIDATION_DEFAULTS \
    {.decay_days = 30,            \
     .decay_factor = 0.9,         \
     .dedup_threshold = 70,       \
     .max_entries = 5000,         \
     .provider = NULL,            \
     .model = NULL,               \
     .model_len = 0}

uint32_t hu_similarity_score(const char *a, size_t a_len, const char *b, size_t b_len);
hu_error_t hu_memory_consolidate(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const hu_consolidation_config_t *config);

/* ── Topic-switch consolidation debounce (inspired by EdgeClaw) ───────── */

#define HU_CONSOLIDATION_MIN_ENTRIES       5
#define HU_CONSOLIDATION_MIN_INTERVAL_SECS 60

typedef struct hu_consolidation_debounce {
    int64_t last_consolidation_secs;
    size_t entries_since_last;
} hu_consolidation_debounce_t;

/* Initialize debounce tracker. */
void hu_consolidation_debounce_init(hu_consolidation_debounce_t *d);

/* Record that a new memory entry was stored. */
void hu_consolidation_debounce_tick(hu_consolidation_debounce_t *d);

/* Check if enough entries and time have elapsed to allow consolidation.
 * Does NOT reset the counters -- call hu_consolidation_debounce_reset after
 * a successful consolidation. */
bool hu_consolidation_should_run(const hu_consolidation_debounce_t *d, int64_t now_secs);

/* Reset counters after a successful consolidation. */
void hu_consolidation_debounce_reset(hu_consolidation_debounce_t *d, int64_t now_secs);

void hu_consolidation_debounce_inject(hu_consolidation_debounce_t *d, size_t extra_ticks);

void hu_consolidation_set_topic_switch(bool detected);
bool hu_consolidation_get_and_clear_topic_switch(void);

#endif
