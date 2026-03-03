#ifndef SC_COST_H
#define SC_COST_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Token usage and cost
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_cost_entry {
    const char *model;
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t total_tokens;
    double cost_usd;
    int64_t timestamp_secs;
} sc_cost_entry_t;

/* Compute cost from token counts and prices (per million). */
void sc_token_usage_init(sc_cost_entry_t *u, const char *model, uint64_t input_tokens,
                         uint64_t output_tokens, double input_price_per_million,
                         double output_price_per_million);

/* ──────────────────────────────────────────────────────────────────────────
 * Usage period
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_usage_period {
    SC_USAGE_PERIOD_SESSION,
    SC_USAGE_PERIOD_DAY,
    SC_USAGE_PERIOD_MONTH,
} sc_usage_period_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Budget check result
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_budget_info {
    double current_usd;
    double limit_usd;
    sc_usage_period_t period;
} sc_budget_info_t;

typedef enum sc_budget_check {
    SC_BUDGET_ALLOWED,
    SC_BUDGET_WARNING,
    SC_BUDGET_EXCEEDED,
} sc_budget_check_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Cost summary
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_cost_summary {
    double session_cost_usd;
    double daily_cost_usd;
    double monthly_cost_usd;
    uint64_t total_tokens;
    size_t request_count;
} sc_cost_summary_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Cost tracker
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_COST_MAX_RECORDS 1024

typedef struct sc_cost_record {
    sc_cost_entry_t usage;
    char model_buf[64];
    char session_id[64];
} sc_cost_record_t;

typedef struct sc_cost_tracker {
    sc_allocator_t *alloc;
    bool enabled;
    double daily_limit_usd;
    double monthly_limit_usd;
    uint32_t warn_at_percent;
    sc_cost_record_t *records;
    size_t record_count;
    size_t record_cap;
    sc_cost_record_t *history;
    size_t history_count;
    size_t history_cap;
    char *storage_path;
} sc_cost_tracker_t;

/* Initialize. storage_path built as workspace_dir/state/costs.jsonl. */
sc_error_t sc_cost_tracker_init(sc_cost_tracker_t *t, sc_allocator_t *alloc,
                                const char *workspace_dir, bool enabled, double daily_limit,
                                double monthly_limit, uint32_t warn_percent);

void sc_cost_tracker_deinit(sc_cost_tracker_t *t);

/* Check if estimated cost is within budget. */
sc_budget_check_t sc_cost_check_budget(const sc_cost_tracker_t *t, double estimated_cost_usd,
                                       sc_budget_info_t *info_out);

/* Record usage. Persists to JSONL if path set. */
sc_error_t sc_cost_record_usage(sc_cost_tracker_t *t, const sc_cost_entry_t *usage);

/* Session cost (in-memory). */
double sc_cost_session_total(const sc_cost_tracker_t *t);

/* Session token count. */
uint64_t sc_cost_session_tokens(const sc_cost_tracker_t *t);

/* Request count. */
size_t sc_cost_request_count(const sc_cost_tracker_t *t);

/* Get summary. */
void sc_cost_get_summary(const sc_cost_tracker_t *t, int64_t at_secs, sc_cost_summary_t *out);

sc_error_t sc_cost_load_history(sc_cost_tracker_t *t);

sc_error_t sc_cost_get_usage_json(sc_allocator_t *alloc, const sc_cost_tracker_t *t,
                                  int64_t at_secs, char **out_json);

#endif /* SC_COST_H */
