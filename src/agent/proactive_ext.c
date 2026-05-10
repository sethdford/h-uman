#include "human/agent/proactive_ext.h"
#include "human/core/string.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_PROACTIVE_EXT_ESCAPE_BUF 512

/* SQL quote escape now lives in core/string.h as hu_sql_quote_escape_into.
 * The local wrapper preserves the void-return signature this module's call
 * sites already expect; callers are intentionally not rewritten here so the
 * dedupe migration stays a one-line code change per file. */
static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap,
                              size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}

static uint32_t lcg_next(uint32_t *seed) {
    *seed = *seed * 1103515245u + 12345u;
    return *seed;
}

/* ─────────────────────────────────────────────────────────────────────────
 * F123 — Reciprocity Throttling
 * ───────────────────────────────────────────────────────────────────────── */

double hu_reciprocity_budget_multiplier(const hu_reciprocity_state_t *state) {
    if (!state)
        return 1.0;
    if (state->unanswered_proactive >= 3)
        return 0.0;
    double m = 1.0;
    if (state->initiation_ratio > 0.7)
        m *= 0.6;
    if (state->contact_just_reengaged)
        m *= 1.5;
    if (m < 0.0)
        m = 0.0;
    if (m > 3.0)
        m = 3.0;
    return m;
}

/* ─────────────────────────────────────────────────────────────────────────
 * F124 — Busyness Simulation
 * ───────────────────────────────────────────────────────────────────────── */

double hu_busyness_budget_multiplier(const hu_busyness_state_t *state) {
    if (!state)
        return 1.0;
    double m = 1.0;
    if (state->calendar_busy)
        m *= 0.5;
    if (state->life_sim_stressed)
        m *= 0.7;
    uint32_t s = state->seed;
    uint32_t r = lcg_next(&s);
    if (((r >> 16) % 100) < 15)
        m *= 0.4;
    if (m < 0.1)
        m = 0.1;
    if (m > 1.0)
        m = 1.0;
    return m;
}

/* ─────────────────────────────────────────────────────────────────────────
 * F129 — "Did I tell you?" Pattern
 * ───────────────────────────────────────────────────────────────────────── */

hu_disclosure_action_t hu_disclosure_decide(double confidence, uint32_t seed) {
    if (confidence < 0.3 || confidence > 0.7)
        return HU_DISCLOSE_TELL_NATURALLY;
    uint32_t s = seed;
    lcg_next(&s);
    uint32_t roll = (s >> 16) % 100;
    if (roll < 40)
        return HU_DISCLOSE_ASK_FIRST;
    if (roll < 70)
        return HU_DISCLOSE_TELL_NATURALLY;
    return HU_DISCLOSE_SKIP;
}

hu_error_t hu_disclosure_build_prefix(hu_allocator_t *alloc,
                                     hu_disclosure_action_t action,
                                     const char *topic, size_t topic_len,
                                     char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (action == HU_DISCLOSE_SKIP || action == HU_DISCLOSE_TELL_NATURALLY)
        return HU_OK;
    if (action != HU_DISCLOSE_ASK_FIRST)
        return HU_OK;
    if (!topic)
        return HU_ERR_INVALID_ARGUMENT;
    static const char prefix[] = "wait did I tell you about ";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t total = prefix_len + topic_len + 2 + 1;
    char *buf = alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, prefix, prefix_len);
    if (topic_len > 0)
        memcpy(buf + prefix_len, topic, topic_len);
    memcpy(buf + prefix_len + topic_len, "? ", 3);
    *out = buf;
    *out_len = prefix_len + topic_len + 2;
    return HU_OK;
}

/* ─────────────────────────────────────────────────────────────────────────
 * F30 — Spontaneous Curiosity
 * ───────────────────────────────────────────────────────────────────────── */

hu_error_t hu_curiosity_query_sql(const char *contact_id, size_t contact_id_len,
                                 uint32_t max_age_days, char *buf, size_t cap,
                                 size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char contact_esc[HU_PROACTIVE_EXT_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc,
                     sizeof(contact_esc), &ce_len);
    uint64_t cutoff_sec = (uint64_t)max_age_days * 86400u;
    int n = snprintf(buf, cap,
                    "SELECT topic, last_mentioned FROM topic_baselines "
                    "WHERE contact_id = '%s' AND last_mentioned > "
                    "(strftime('%%s','now') - %llu) ORDER BY last_mentioned DESC",
                    contact_esc, (unsigned long long)cutoff_sec);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

double hu_curiosity_score(uint32_t days_since_mentioned, uint32_t times_discussed,
                          bool we_brought_it_up_last) {
    double base = 0.5;
    if (days_since_mentioned >= 3 && days_since_mentioned <= 7)
        base = 0.9;
    else if (days_since_mentioned == 1)
        base = 0.4;
    else if (days_since_mentioned > 14)
        base = 0.3;
    if (times_discussed > 3)
        base *= 0.5;
    if (we_brought_it_up_last)
        base *= 0.3;
    return base;
}

void hu_curiosity_topic_deinit(hu_allocator_t *alloc,
                               hu_curiosity_topic_t *t) {
    if (!alloc || !t)
        return;
    hu_str_free(alloc, t->topic);
    hu_str_free(alloc, t->prompt);
    t->topic = NULL;
    t->prompt = NULL;
    t->topic_len = 0;
    t->prompt_len = 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * F31 — Callback Opportunities
 * ───────────────────────────────────────────────────────────────────────── */

hu_error_t hu_callback_query_sql(const char *contact_id, size_t contact_id_len,
                                 uint64_t min_age_ms, uint64_t max_age_ms,
                                 char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char contact_esc[HU_PROACTIVE_EXT_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc,
                     sizeof(contact_esc), &ce_len);
    uint64_t min_sec = min_age_ms / 1000u;
    uint64_t max_sec = max_age_ms / 1000u;
    int n = snprintf(buf, cap,
                    "SELECT fact, significance, created_at FROM micro_moments "
                    "WHERE contact_id = '%s' AND created_at BETWEEN "
                    "(strftime('%%s','now') - %llu) AND "
                    "(strftime('%%s','now') - %llu) ORDER BY created_at DESC",
                    contact_esc, (unsigned long long)max_sec,
                    (unsigned long long)min_sec);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

double hu_callback_score(uint32_t days_old, double emotional_weight,
                         bool was_important) {
    double base = 0.5;
    if (days_old >= 3 && days_old <= 7)
        base = 0.9;
    double m = base * emotional_weight;
    if (was_important)
        m += 0.2;
    if (m < 0.0)
        m = 0.0;
    if (m > 1.0)
        m = 1.0;
    return m;
}

void hu_callback_opportunity_deinit(hu_allocator_t *alloc,
                                    hu_callback_opportunity_t *c) {
    if (!alloc || !c)
        return;
    hu_str_free(alloc, c->topic);
    hu_str_free(alloc, c->original_context);
    c->topic = NULL;
    c->original_context = NULL;
    c->topic_len = 0;
    c->original_context_len = 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Build prompt for curiosity/callback injection
 * ───────────────────────────────────────────────────────────────────────── */

hu_error_t hu_proactive_ext_build_prompt(
    hu_allocator_t *alloc, const hu_curiosity_topic_t *curiosity,
    size_t curiosity_count, const hu_callback_opportunity_t *callbacks,
    size_t callback_count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (curiosity_count == 0 && callback_count == 0)
        return HU_OK;

    size_t cap = 1024;
    char *buf = alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;

    static const char header[] = "[FOLLOW-UP OPPORTUNITIES]:\n";
    size_t hlen = sizeof(header) - 1;
    if (pos + hlen >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf, header, hlen + 1);
    pos = hlen;

    for (size_t i = 0; i < curiosity_count && curiosity; i++) {
        const hu_curiosity_topic_t *c = &curiosity[i];
        const char *topic = c->topic ? c->topic : "(topic)";
        size_t topic_len = c->topic_len;
        const char *prompt = c->prompt ? c->prompt : "";
        size_t prompt_len = c->prompt_len;
        int n = snprintf(buf + pos, cap - pos,
                        "Curiosity: %.*s (relevance: %.2f) — \"%.*s\"\n",
                        (int)topic_len, topic, c->relevance,
                        (int)prompt_len, prompt);
        if (n < 0 || pos + (size_t)n >= cap) {
            alloc->free(alloc->ctx, buf, cap);
            return HU_ERR_INVALID_ARGUMENT;
        }
        pos += (size_t)n;
    }

    for (size_t i = 0; i < callback_count && callbacks; i++) {
        const hu_callback_opportunity_t *cb = &callbacks[i];
        const char *topic = cb->topic ? cb->topic : "(topic)";
        size_t topic_len = cb->topic_len;
        int n = snprintf(buf + pos, cap - pos,
                        "Callback: %.*s (score: %.2f) — check in on the result\n",
                        (int)topic_len, topic, cb->callback_score);
        if (n < 0 || pos + (size_t)n >= cap) {
            alloc->free(alloc->ctx, buf, cap);
            return HU_ERR_INVALID_ARGUMENT;
        }
        pos += (size_t)n;
    }

    static const char footer[] = "\nPick one to weave in naturally if the moment is right.\n";
    size_t flen = sizeof(footer) - 1;
    if (pos + flen >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf + pos, footer, flen + 1);
    pos += flen;

    *out = buf;
    *out_len = pos;
    return HU_OK;
}
