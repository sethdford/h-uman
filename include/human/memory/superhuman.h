#ifndef HU_MEMORY_SUPERHUMAN_H
#define HU_MEMORY_SUPERHUMAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Phase 3 superhuman memory types (fixed-size arrays, no per-field allocation)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_inside_joke {
    int64_t id;
    char contact_id[128];
    char context[512];
    char punchline[256];
    int64_t created_at;
    int64_t last_referenced;
    uint32_t reference_count;
} hu_inside_joke_t;

typedef struct hu_superhuman_commitment {
    int64_t id;
    char contact_id[128];
    char description[512];
    char who[64];
    int64_t deadline;
    char status[32];
    int64_t created_at;
    int64_t followed_up_at;
} hu_superhuman_commitment_t;

typedef struct hu_delayed_followup {
    int64_t id;
    char contact_id[128];
    char topic[256];
    int64_t scheduled_at;
    bool sent;
} hu_delayed_followup_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Inside jokes
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_inside_joke_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *context, size_t context_len,
    const char *punchline, size_t punchline_len);
hu_error_t hu_superhuman_inside_joke_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit,
    hu_inside_joke_t **out, size_t *out_count);
hu_error_t hu_superhuman_inside_joke_reference(void *sqlite_ctx, int64_t id);
void hu_superhuman_inside_joke_free(hu_allocator_t *alloc, hu_inside_joke_t *arr, size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * Commitments
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_commitment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *description, size_t desc_len,
    const char *who, size_t who_len, int64_t deadline);
hu_error_t hu_superhuman_commitment_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, size_t limit, hu_superhuman_commitment_t **out, size_t *out_count);
hu_error_t hu_superhuman_commitment_mark_followed_up(void *sqlite_ctx, int64_t id);
void hu_superhuman_commitment_free(hu_allocator_t *alloc, hu_superhuman_commitment_t *arr,
    size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * Temporal patterns
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_temporal_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, int day_of_week, int hour, int64_t response_time_ms);
hu_error_t hu_superhuman_temporal_get_quiet_hours(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int *out_day, int *out_hour_start,
    int *out_hour_end);

/* ──────────────────────────────────────────────────────────────────────────
 * Delayed follow-ups
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_delayed_followup_schedule(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    int64_t scheduled_at);
hu_error_t hu_superhuman_delayed_followup_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, hu_delayed_followup_t **out, size_t *out_count);
hu_error_t hu_superhuman_delayed_followup_mark_sent(void *sqlite_ctx, int64_t id);
void hu_superhuman_delayed_followup_free(hu_allocator_t *alloc, hu_delayed_followup_t *arr,
    size_t count);

/* ──────────────────────────────────────────────────────────────────────────
 * Micro-moments
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_micro_moment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *fact, size_t fact_len,
    const char *significance, size_t sig_len);
hu_error_t hu_superhuman_micro_moment_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Avoidance patterns
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_avoidance_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, bool topic_changed_quickly);
hu_error_t hu_superhuman_avoidance_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, char **out_json, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Topic baselines
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_topic_baseline_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len);
hu_error_t hu_superhuman_topic_absence_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int64_t now_ts, int64_t absence_days,
    char **out_json, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Growth milestones
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_growth_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    const char *before_state, size_t before_len, const char *after_state, size_t after_len);
hu_error_t hu_superhuman_growth_list_recent(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len);

/* ──────────────────────────────────────────────────────────────────────────
 * Pattern observations
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_superhuman_pattern_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, const char *tone, size_t tone_len,
    int day_of_week, int hour);
hu_error_t hu_superhuman_pattern_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len);

hu_error_t hu_superhuman_memory_build_context(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, bool include_avoidance,
    char **out, size_t *out_len);

#endif /* HU_MEMORY_SUPERHUMAN_H */
