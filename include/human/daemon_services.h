#ifndef HU_DAEMON_SERVICES_H
#define HU_DAEMON_SERVICES_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Daemon Services Vtable — Decouples daemon.c from implementation details
 * ────────────────────────────────────────────────────────────────────────── */

/* Forward declarations to avoid circular includes */
typedef struct hu_channel hu_channel_t;
typedef struct hu_provider hu_provider_t;

/* Consolidation debounce tracker — moved here to avoid impl header dependency */
typedef struct hu_consolidation_debounce {
    int64_t last_consolidation_secs;
    size_t entries_since_last;
} hu_consolidation_debounce_t;

/* Consolidation configuration — moved here to avoid impl header dependency */
typedef struct hu_consolidation_config {
    uint32_t decay_days;
    double decay_factor;
    uint32_t dedup_threshold; /* 0-100 token overlap percentage */
    uint32_t max_entries;
    hu_provider_t *provider; /* optional; NULL = skip connection discovery */
    const char *model;       /* model name for LLM calls; NULL uses provider default */
    size_t model_len;
    bool extract_facts; /* run deep_extract on surviving entries and store as propositions */
    float
        fact_confidence_threshold; /* minimum confidence for stored facts (0.0-1.0, default 0.5) */
} hu_consolidation_config_t;

#define HU_CONSOLIDATION_DEFAULTS \
    {.decay_days = 30,            \
     .decay_factor = 0.9,         \
     .dedup_threshold = 85,       \
     .max_entries = 10000,        \
     .provider = NULL,            \
     .model = NULL,               \
     .model_len = 0,              \
     .extract_facts = false,      \
     .fact_confidence_threshold = 0.5f}

/* Consolidation service — memory optimization */
typedef struct hu_daemon_memory_svc {
    /* Run scheduled consolidation (nightly/weekly/monthly) */
    hu_error_t (*consolidate_scheduled)(void *ctx, int64_t now_ts, int64_t last_nightly,
                                        int64_t last_weekly, int64_t last_monthly);

    /* Manually trigger consolidation (full pass) */
    hu_error_t (*consolidate_full)(void *ctx, hu_allocator_t *alloc, hu_memory_t *memory,
                                   const hu_consolidation_config_t *config);

    /* Topic-switch consolidation debounce operations */
    void (*debounce_init)(hu_consolidation_debounce_t *d);
    void (*debounce_tick)(hu_consolidation_debounce_t *d);
    void (*debounce_reset)(hu_consolidation_debounce_t *d, int64_t now_secs);
    bool (*debounce_should_run)(const hu_consolidation_debounce_t *d, int64_t now_secs);
    void (*debounce_set_topic_switch)(bool detected);
} hu_daemon_memory_svc_t;

/* iMessage service — channel-specific operations */
typedef struct hu_daemon_imessage_svc {
    /* Look up original message by GUID (for reply threading) */
    hu_error_t (*lookup_message_by_guid)(hu_allocator_t *alloc, const char *guid, size_t guid_len,
                                         char *text_out, size_t text_out_size, size_t *text_len);

    /* Fetch GIF from query (async/deferred) */
    char *(*fetch_gif)(hu_allocator_t *alloc, const char *query, size_t query_len,
                       const char *api_key, size_t api_key_len);

    /* Count recent GIF tapbacks (emoji reactions) */
    size_t (*count_recent_gif_tapbacks)(void);
} hu_daemon_imessage_svc_t;

/* Fast capture service — topic/emotion/entity extraction */
/* Forward declare to avoid pulling in memory/fast_capture.h */
typedef struct hu_fc_result hu_fc_result_t;

typedef struct hu_daemon_fast_capture_svc {
    /* Fast extraction of topic, entities, emotion from text */
    hu_error_t (*capture)(hu_allocator_t *alloc, const char *text, size_t text_len,
                          hu_fc_result_t *result_out);

    /* Clean up result allocations */
    void (*result_deinit)(hu_fc_result_t *result, hu_allocator_t *alloc);
} hu_daemon_fast_capture_svc_t;

/* Master daemon services registry */
typedef struct hu_daemon_services {
    hu_daemon_memory_svc_t *memory;
    void *memory_ctx;

    hu_daemon_imessage_svc_t *imessage;
    void *imessage_ctx;

    hu_daemon_fast_capture_svc_t *fast_capture;
    void *fast_capture_ctx;
} hu_daemon_services_t;

#endif /* HU_DAEMON_SERVICES_H */
