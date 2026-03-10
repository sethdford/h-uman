#ifndef HU_COST_H
#define HU_COST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Token usage and cost
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_cost_entry {
    const char *model;
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t total_tokens;
    double cost_usd;
    int64_t timestamp_secs;
} hu_cost_entry_t;

/* Compute cost from token counts and prices (per million). */
void hu_token_usage_init(hu_cost_entry_t *u, const char *model, uint64_t input_tokens,
                         uint64_t output_tokens, double input_price_per_million,
                         double output_price_per_million);

/* ──────────────────────────────────────────────────────────────────────────
 * Usage period
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_usage_period {
    HU_USAGE_PERIOD_SESSION,
    HU_USAGE_PERIOD_DAY,
    HU_USAGE_PERIOD_MONTH,
} hu_usage_period_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Budget check result
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_budget_info {
    double current_usd;
    double limit_usd;
    hu_usage_period_t period;
} hu_budget_info_t;

typedef enum hu_budget_check {
    HU_BUDGET_ALLOWED,
    HU_BUDGET_WARNING,
    HU_BUDGET_EXCEEDED,
} hu_budget_check_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Cost summary
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_cost_summary {
    double session_cost_usd;
    double daily_cost_usd;
    double monthly_cost_usd;
    uint64_t total_tokens;
    size_t request_count;
} hu_cost_summary_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Cost tracker
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_COST_MAX_RECORDS 1024

typedef struct hu_cost_record {
    hu_cost_entry_t usage;
    char model_buf[64];
    char session_id[64];
    uint64_t job_id; /* 0 = interactive, >0 = automation/cron job ID */
} hu_cost_record_t;

typedef struct hu_cost_tracker {
    hu_allocator_t *alloc;
    bool enabled;
    double daily_limit_usd;
    double monthly_limit_usd;
    uint32_t warn_at_percent;
    hu_cost_record_t *records;
    size_t record_count;
    size_t record_cap;
    hu_cost_record_t *history;
    size_t history_count;
    size_t history_cap;
    char *storage_path;
} hu_cost_tracker_t;

/* Initialize. storage_path built as workspace_dir/state/costs.jsonl. */
hu_error_t hu_cost_tracker_init(hu_cost_tracker_t *t, hu_allocator_t *alloc,
                                const char *workspace_dir, bool enabled, double daily_limit,
                                double monthly_limit, uint32_t warn_percent);

void hu_cost_tracker_deinit(hu_cost_tracker_t *t);

/* Check if estimated cost is within budget. */
hu_budget_check_t hu_cost_check_budget(const hu_cost_tracker_t *t, double estimated_cost_usd,
                                       hu_budget_info_t *info_out);

/* Record usage. Persists to JSONL if path set. job_id: 0 = interactive, >0 = automation/cron. */
hu_error_t hu_cost_record_usage(hu_cost_tracker_t *t, const hu_cost_entry_t *usage,
                               uint64_t job_id);

/* Session cost (in-memory). */
double hu_cost_session_total(const hu_cost_tracker_t *t);

/* Session token count. */
uint64_t hu_cost_session_tokens(const hu_cost_tracker_t *t);

/* Request count. */
size_t hu_cost_request_count(const hu_cost_tracker_t *t);

/* Get summary. */
void hu_cost_get_summary(const hu_cost_tracker_t *t, int64_t at_secs, hu_cost_summary_t *out);

hu_error_t hu_cost_load_history(hu_cost_tracker_t *t);

hu_error_t hu_cost_get_usage_json(hu_allocator_t *alloc, const hu_cost_tracker_t *t,
                                  int64_t at_secs, char **out_json);

#endif /* HU_COST_H */
