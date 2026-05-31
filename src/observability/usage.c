#include "human/usage.h"
#include "human/core/string.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Pricing table for major providers (approximate, from public pricing)
 * ────────────────────────────────────────────────────────────────────────── */

static const hu_model_pricing_t HU_PRICING_TABLE[] = {
    /* Anthropic Claude */
    {"claude-opus-4*",        15.0,  75.0,   0.0,   0.0},
    {"claude-sonnet-4",        3.0,  15.0,   0.0,   0.0},
    {"claude-3.5-sonnet",      3.0,  15.0,   0.0,   0.0},
    {"claude-haiku",           0.25,  1.25,  0.0,   0.0},
    {"claude-3-opus",         15.0,  75.0,   0.0,   0.0},
    {"claude-3-sonnet",        3.0,  15.0,   0.0,   0.0},
    {"claude-3-haiku",         0.25,  1.25,  0.0,   0.0},

    /* OpenAI */
    {"gpt-4o",                 2.5,  10.0,   1.25,  5.0},
    {"gpt-4o-mini",            0.15,  0.6,   0.075, 0.3},
    {"gpt-4.1",                2.0,   8.0,   0.0,   0.0},
    {"gpt-4.1-mini",           0.4,   1.6,   0.0,   0.0},
    {"gpt-4-turbo",            0.01,  0.03,  0.0,   0.0},
    {"gpt-4",                  0.03,  0.06,  0.0,   0.0},
    {"gpt-3.5-turbo",          0.0005, 0.0015, 0.0, 0.0},

    /* Google Gemini */
    {"gemini-3.1-pro",         1.25,  5.0,   0.0,   0.0},
    {"gemini-3.1-flash",       0.075, 0.3,   0.0,   0.0},
    {"gemini-3.1-flash-lite",  0.075, 0.3,   0.0,   0.0},
    {"gemini-pro",             0.5,   1.5,   0.0,   0.0},

    /* OpenAI Reasoning */
    {"o3",                    10.0,  40.0,   0.0,   0.0},
    {"o3-mini",                0.8,   3.2,   0.0,   0.0},
    {"o4-mini",                1.1,   4.4,   0.0,   0.0},

    /* Anthropic Haiku with thinking */
    {"claude-haiku-thinking",  0.8,   3.2,   0.0,   0.0},

    /* Sentinel */
    {NULL, 0, 0, 0, 0}
};

/* ──────────────────────────────────────────────────────────────────────────
 * Model usage entry (tracks one model's usage)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_model_usage_entry {
    char model_name[256];
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t cache_read_tokens;
    uint64_t cache_write_tokens;
    size_t request_count;
} hu_model_usage_entry_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Usage tracker implementation
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_USAGE_TRACKER_MAX_MODELS 256

struct hu_usage_tracker {
    hu_allocator_t *alloc;
    hu_model_usage_entry_t models[HU_USAGE_TRACKER_MAX_MODELS];
    size_t model_count;
};

/* Find matching pricing entry for a model name (prefix matching) */
static const hu_model_pricing_t *hu_usage_find_pricing(const char *model) {
    if (!model)
        return NULL;

    for (size_t i = 0; HU_PRICING_TABLE[i].model_pattern; i++) {
        const char *pattern = HU_PRICING_TABLE[i].model_pattern;
        size_t pattern_len = strlen(pattern);

        /* Check if pattern ends with "*" */
        if (pattern_len > 0 && pattern[pattern_len - 1] == '*') {
            /* Prefix match */
            size_t prefix_len = pattern_len - 1;
            if (strncmp(model, pattern, prefix_len) == 0)
                return &HU_PRICING_TABLE[i];
        } else {
            /* Exact match */
            if (strcmp(model, pattern) == 0)
                return &HU_PRICING_TABLE[i];
        }
    }

    return NULL;
}

/* Calculate cost for a single model's usage */
static double hu_usage_calculate_cost(const hu_model_usage_entry_t *entry,
                                      const hu_model_pricing_t *pricing) {
    if (!entry || !pricing)
        return 0.0;

    double cost = 0.0;
    cost += (double)entry->input_tokens / 1000000.0 * pricing->input_per_mtok;
    cost += (double)entry->output_tokens / 1000000.0 * pricing->output_per_mtok;
    cost += (double)entry->cache_read_tokens / 1000000.0 * pricing->cache_read_per_mtok;
    cost += (double)entry->cache_write_tokens / 1000000.0 * pricing->cache_write_per_mtok;
    return cost;
}

/* Find or create model usage entry */
static hu_model_usage_entry_t *hu_usage_find_or_create_model(hu_usage_tracker_t *tracker,
                                                              const char *model) {
    if (!tracker || !model)
        return NULL;

    /* Look for existing entry */
    for (size_t i = 0; i < tracker->model_count; i++) {
        if (strcmp(tracker->models[i].model_name, model) == 0)
            return &tracker->models[i];
    }

    /* Create new entry */
    if (tracker->model_count >= HU_USAGE_TRACKER_MAX_MODELS)
        return NULL;

    size_t idx = tracker->model_count++;
    hu_model_usage_entry_t *entry = &tracker->models[idx];
    memset(entry, 0, sizeof(*entry));
    /* Ensure NUL-termination after strncpy (truncation produces collisions). */
    strncpy(entry->model_name, model, sizeof(entry->model_name) - 1);
    entry->model_name[sizeof(entry->model_name) - 1] = '\0';
    return entry;
}

hu_error_t hu_usage_tracker_create(hu_allocator_t *alloc, hu_usage_tracker_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_usage_tracker_t *tracker =
        (hu_usage_tracker_t *)alloc->alloc(alloc->ctx, sizeof(hu_usage_tracker_t));
    if (!tracker)
        return HU_ERR_OUT_OF_MEMORY;

    memset(tracker, 0, sizeof(*tracker));
    tracker->alloc = alloc;
    *out = tracker;
    return HU_OK;
}

void hu_usage_tracker_destroy(hu_usage_tracker_t *tracker) {
    if (!tracker || !tracker->alloc)
        return;
    tracker->alloc->free(tracker->alloc->ctx, tracker, sizeof(hu_usage_tracker_t));
}

hu_error_t hu_usage_tracker_record(hu_usage_tracker_t *tracker,
                                    const char *model, const hu_extended_token_usage_t *usage) {
    if (!tracker || !model || !usage)
        return HU_ERR_INVALID_ARGUMENT;

    hu_model_usage_entry_t *entry = hu_usage_find_or_create_model(tracker, model);
    if (!entry)
        return HU_ERR_OUT_OF_MEMORY;

    entry->input_tokens += usage->input_tokens;
    entry->output_tokens += usage->output_tokens;
    entry->cache_read_tokens += usage->cache_read_tokens;
    entry->cache_write_tokens += usage->cache_write_tokens;
    entry->request_count++;

    return HU_OK;
}

hu_error_t hu_usage_tracker_get_totals(const hu_usage_tracker_t *tracker,
                                        hu_extended_token_usage_t *out) {
    if (!tracker || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < tracker->model_count; i++) {
        out->input_tokens += tracker->models[i].input_tokens;
        out->output_tokens += tracker->models[i].output_tokens;
        out->cache_read_tokens += tracker->models[i].cache_read_tokens;
        out->cache_write_tokens += tracker->models[i].cache_write_tokens;
    }
    return HU_OK;
}

double hu_usage_tracker_estimate_cost(const hu_usage_tracker_t *tracker) {
    if (!tracker)
        return 0.0;

    double total_cost = 0.0;
    for (size_t i = 0; i < tracker->model_count; i++) {
        const hu_model_pricing_t *pricing =
            hu_usage_find_pricing(tracker->models[i].model_name);
        if (pricing) {
            total_cost += hu_usage_calculate_cost(&tracker->models[i], pricing);
        }
    }
    return total_cost;
}

hu_error_t hu_usage_tracker_get_breakdown(const hu_usage_tracker_t *tracker,
                                          hu_allocator_t *alloc,
                                          hu_model_usage_t **out, size_t *out_count) {
    if (!tracker || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    if (tracker->model_count == 0) {
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    hu_model_usage_t *breakdown =
        (hu_model_usage_t *)alloc->alloc(alloc->ctx, tracker->model_count * sizeof(hu_model_usage_t));
    if (!breakdown)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < tracker->model_count; i++) {
        const hu_model_usage_entry_t *entry = &tracker->models[i];
        hu_model_usage_t *out_entry = &breakdown[i];

        memset(out_entry, 0, sizeof(*out_entry));
        strncpy(out_entry->model_name, entry->model_name, sizeof(out_entry->model_name) - 1);
        out_entry->model_name[sizeof(out_entry->model_name) - 1] = '\0';
        out_entry->input_tokens = entry->input_tokens;
        out_entry->output_tokens = entry->output_tokens;
        out_entry->cache_read_tokens = entry->cache_read_tokens;
        out_entry->cache_write_tokens = entry->cache_write_tokens;
        out_entry->request_count = entry->request_count;

        const hu_model_pricing_t *pricing = hu_usage_find_pricing(entry->model_name);
        if (pricing) {
            out_entry->estimated_cost_usd = hu_usage_calculate_cost(entry, pricing);
        }
    }

    *out = breakdown;
    *out_count = tracker->model_count;
    return HU_OK;
}

hu_error_t hu_usage_tracker_format_report(const hu_usage_tracker_t *tracker,
                                           hu_allocator_t *alloc,
                                           char **out, size_t *out_len) {
    if (!tracker || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_model_usage_t *breakdown = NULL;
    size_t breakdown_count = 0;
    hu_error_t err = hu_usage_tracker_get_breakdown(tracker, alloc, &breakdown, &breakdown_count);
    if (err != HU_OK)
        return err;

    if (breakdown_count == 0) {
        *out = hu_strndup(alloc, "No usage recorded.", 18);
        *out_len = *out ? 18 : 0;
        return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    /* Calculate totals */
    hu_extended_token_usage_t totals;
    hu_usage_tracker_get_totals(tracker, &totals);
    double total_cost = hu_usage_tracker_estimate_cost(tracker);

    /* Format report */
    char buffer[8192];
    size_t offset = 0;

    /* Header */
    offset = hu_buf_appendf(buffer, sizeof(buffer), offset,
                            "Session cost: $%.4f\n", total_cost);

    /* Per-model breakdown */
    for (size_t i = 0; i < breakdown_count; i++) {
        const hu_model_usage_t *mu = &breakdown[i];
        offset = hu_buf_appendf(
            buffer, sizeof(buffer), offset,
            "  %s: %llu in / %llu out", mu->model_name,
            (unsigned long long)mu->input_tokens, (unsigned long long)mu->output_tokens);

        if (mu->cache_read_tokens > 0 || mu->cache_write_tokens > 0) {
            offset = hu_buf_appendf(buffer, sizeof(buffer), offset,
                                    " / %llu cache_r / %llu cache_w",
                                    (unsigned long long)mu->cache_read_tokens,
                                    (unsigned long long)mu->cache_write_tokens);
        }

        offset = hu_buf_appendf(buffer, sizeof(buffer), offset,
                                " ($%.4f, %zu req)\n",
                                mu->estimated_cost_usd, mu->request_count);
    }

    /* Totals */
    offset = hu_buf_appendf(buffer, sizeof(buffer), offset,
                            "Total: %llu input / %llu output tokens",
                            (unsigned long long)totals.input_tokens,
                            (unsigned long long)totals.output_tokens);

    if (totals.cache_read_tokens > 0 || totals.cache_write_tokens > 0) {
        offset = hu_buf_appendf(buffer, sizeof(buffer), offset,
                                " / %llu cache_read / %llu cache_write",
                                (unsigned long long)totals.cache_read_tokens,
                                (unsigned long long)totals.cache_write_tokens);
    }
    offset = hu_buf_appendf(buffer, sizeof(buffer), offset, "\n");

    *out = hu_strndup(alloc, buffer, offset);
    *out_len = *out ? offset : 0;

    /* Free breakdown */
    if (breakdown) {
        alloc->free(alloc->ctx, breakdown, breakdown_count * sizeof(hu_model_usage_t));
    }

    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

void hu_usage_tracker_reset(hu_usage_tracker_t *tracker) {
    if (!tracker)
        return;
    memset(tracker->models, 0, sizeof(tracker->models));
    tracker->model_count = 0;
}

size_t hu_usage_tracker_request_count(const hu_usage_tracker_t *tracker) {
    if (!tracker)
        return 0;

    size_t count = 0;
    for (size_t i = 0; i < tracker->model_count; i++) {
        count += tracker->models[i].request_count;
    }
    return count;
}
