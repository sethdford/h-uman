#include "human/agent/governor.h"
#include <stdlib.h>
#include <string.h>

#define HU_GOVERNOR_MS_PER_DAY 86400000ULL

static const hu_proactive_budget_config_t HU_GOVERNOR_DEFAULTS = {
    .daily_max = 3,
    .weekly_max = 10,
    .relationship_multiplier = 1.0,
    .cool_off_after_unanswered = 2,
    .cool_off_hours = 72,
};

hu_error_t hu_governor_init(const hu_proactive_budget_config_t *config,
                            hu_proactive_budget_t *out)
{
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_proactive_budget_config_t *c = config;
    if (c) {
        if (c->daily_max == 0)
            return HU_ERR_INVALID_ARGUMENT;
        if (c->weekly_max < c->daily_max)
            return HU_ERR_INVALID_ARGUMENT;
    } else {
        c = &HU_GOVERNOR_DEFAULTS;
    }

    memset(out, 0, sizeof(*out));
    out->daily_max = c->daily_max;
    out->weekly_max = c->weekly_max;
    out->relationship_multiplier = c->relationship_multiplier;
    out->cool_off_after_unanswered = c->cool_off_after_unanswered;
    out->cool_off_hours = c->cool_off_hours;
    return HU_OK;
}

static void apply_resets(hu_proactive_budget_t *budget, uint64_t now_ms)
{
    uint64_t day_number = now_ms / HU_GOVERNOR_MS_PER_DAY;
    uint64_t week_number = day_number / 7;

    if (day_number > budget->last_reset_day) {
        budget->daily_used = 0;
        budget->last_reset_day = day_number;
    }
    if (week_number > budget->last_reset_week) {
        budget->weekly_used = 0;
        budget->last_reset_week = week_number;
    }
}

typedef struct {
    size_t idx;
    double priority;
} hu_governor_priority_entry_t;

static int compare_priority_desc(const void *a, const void *b)
{
    const hu_governor_priority_entry_t *pa = (const hu_governor_priority_entry_t *)a;
    const hu_governor_priority_entry_t *pb = (const hu_governor_priority_entry_t *)b;
    if (pa->priority > pb->priority)
        return -1;
    if (pa->priority < pb->priority)
        return 1;
    return 0;
}

#define HU_GOVERNOR_MAX_FILTER 64

hu_error_t hu_governor_filter_by_priority(hu_proactive_budget_t *budget,
                                          const double *priorities, size_t count,
                                          bool *allowed, size_t *out_allowed_count,
                                          uint64_t now_ms)
{
    if (!budget || !out_allowed_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (count > 0 && (!priorities || !allowed))
        return HU_ERR_INVALID_ARGUMENT;
    if (count > HU_GOVERNOR_MAX_FILTER)
        return HU_ERR_INVALID_ARGUMENT;

    *out_allowed_count = 0;

    if (budget->cool_off_until > now_ms) {
        if (count > 0)
            for (size_t i = 0; i < count; i++)
                allowed[i] = false;
        return HU_OK;
    }

    apply_resets(budget, now_ms);

    uint8_t effective_daily = hu_governor_effective_daily_max(budget);
    uint8_t daily_remaining = budget->daily_used >= effective_daily
                                  ? 0
                                  : (uint8_t)(effective_daily - budget->daily_used);
    uint8_t weekly_remaining = budget->weekly_used >= budget->weekly_max
                                  ? 0
                                  : (uint8_t)(budget->weekly_max - budget->weekly_used);
    uint8_t remaining = daily_remaining < weekly_remaining ? daily_remaining : weekly_remaining;

    if (remaining >= count) {
        for (size_t i = 0; i < count; i++)
            allowed[i] = true;
        *out_allowed_count = count;
        return HU_OK;
    }

    hu_governor_priority_entry_t entries[HU_GOVERNOR_MAX_FILTER];
    for (size_t i = 0; i < count; i++) {
        entries[i].idx = i;
        entries[i].priority = priorities[i];
    }
    qsort(entries, count, sizeof(entries[0]), compare_priority_desc);

    for (size_t i = 0; i < count; i++)
        allowed[i] = false;
    for (size_t i = 0; i < remaining && i < count; i++)
        allowed[entries[i].idx] = true;
    *out_allowed_count = remaining;
    return HU_OK;
}

hu_error_t hu_governor_record_sent(hu_proactive_budget_t *budget, uint64_t now_ms)
{
    if (!budget)
        return HU_ERR_INVALID_ARGUMENT;

    apply_resets(budget, now_ms);

    budget->daily_used++;
    budget->weekly_used++;
    budget->unanswered_count++;

    if (budget->unanswered_count >= budget->cool_off_after_unanswered) {
        budget->cool_off_until = now_ms + (uint64_t)budget->cool_off_hours * 3600000ULL;
    }
    return HU_OK;
}

hu_error_t hu_governor_record_response(hu_proactive_budget_t *budget)
{
    if (!budget)
        return HU_ERR_INVALID_ARGUMENT;
    budget->unanswered_count = 0;
    budget->cool_off_until = 0;
    return HU_OK;
}

bool hu_governor_has_budget(const hu_proactive_budget_t *budget, uint64_t now_ms)
{
    if (!budget)
        return false;
    if (budget->cool_off_until > now_ms)
        return false;
    uint8_t effective = hu_governor_effective_daily_max(budget);
    return budget->daily_used < effective && budget->weekly_used < budget->weekly_max;
}

uint8_t hu_governor_effective_daily_max(const hu_proactive_budget_t *budget)
{
    if (!budget)
        return 1;
    double v = (double)budget->daily_max * budget->relationship_multiplier;
    uint8_t r = (uint8_t)v;
    return r == 0 ? 1 : r;
}

double hu_governor_compute_priority(uint8_t urgency_0_to_10,
                                    uint8_t relevance_0_to_10,
                                    uint8_t freshness_0_to_10,
                                    uint8_t social_debt_0_to_10,
                                    uint8_t emotional_weight_0_to_10)
{
    double u = (double)urgency_0_to_10 / 10.0;
    double r = (double)relevance_0_to_10 / 10.0;
    double f = (double)freshness_0_to_10 / 10.0;
    double s = (double)social_debt_0_to_10 / 10.0;
    double e = (double)emotional_weight_0_to_10 / 10.0;
    double score = u * 0.30 + r * 0.25 + f * 0.20 + s * 0.15 + e * 0.10;
    if (score < 0.0)
        return 0.0;
    if (score > 1.0)
        return 1.0;
    return score;
}
