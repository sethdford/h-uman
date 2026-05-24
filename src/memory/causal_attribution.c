/* src/memory/causal_attribution.c
 *
 * Pure read-only scan over personal_model reaction-derived facts.
 * Sprint B Story 6 (docs/plans/2026-05-19-sprint-backlog.md). */

#include "human/memory/causal_attribution.h"

#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Word-boundary case-insensitive match (shared with anticipatory /
 * emotional_context; duplicated to keep module dep-free). */
static bool word_match(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle)
        return false;
    size_t hlen = strlen(hay), nlen = strlen(needle);
    if (hlen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(hay + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || !isalnum((unsigned char)hay[i - 1]);
        bool right_ok = (i + nlen == hlen) || !isalnum((unsigned char)hay[i + nlen]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

/* Positive verbs the reaction-ingest pipeline writes into fact->predicate
 * when the user reacted with a positive emoji/tapback. Word-boundary
 * matched to avoid "dislikes" triggering "likes". */
static const char *const k_positive_verbs[] = {"loves", "likes", "appreciates", "enjoys", NULL};
static const char *const k_negative_verbs[] = {"hates", "dislikes", "resents", NULL};

static bool fact_is_positive(const hu_heuristic_fact_t *f) {
    for (size_t i = 0; k_positive_verbs[i]; i++)
        if (word_match(f->predicate, k_positive_verbs[i]))
            return true;
    return false;
}

static bool fact_is_negative(const hu_heuristic_fact_t *f) {
    for (size_t i = 0; k_negative_verbs[i]; i++)
        if (word_match(f->predicate, k_negative_verbs[i]))
            return true;
    return false;
}

size_t hu_causal_attribution_summarize(const struct hu_personal_model *model_opaque,
                                       const char *contact_handle,
                                       hu_causal_attribution_summary_t *out) {
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!model_opaque || !contact_handle || !*contact_handle)
        return 0;

    const hu_personal_model_t *model = (const hu_personal_model_t *)model_opaque;
    if (model->fact_count == 0)
        return 0;

    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];
        /* Only count reaction-derived facts. */
        if (strncasecmp(f->source_hint, "reaction_ingest", HU_FACT_MAX_FIELD) != 0)
            continue;
        /* Only the requested contact. */
        if (strncasecmp(f->provenance.contact_handle, contact_handle, HU_PROV_HANDLE_MAX) != 0)
            continue;

        out->total_reactions++;
        if (fact_is_positive(f))
            out->positive_count++;
        else if (fact_is_negative(f))
            out->negative_count++;
        else
            out->neutral_count++;

        if (out->earliest_seen == 0 || f->last_seen_at < out->earliest_seen)
            out->earliest_seen = f->last_seen_at;
        if (f->last_seen_at > out->latest_seen)
            out->latest_seen = f->last_seen_at;
    }
    return (size_t)out->total_reactions;
}

size_t hu_causal_attribution_render(const char *contact_handle,
                                    const hu_causal_attribution_summary_t *summary, int64_t now,
                                    char *out, size_t cap) {
    if (!contact_handle || !*contact_handle || !summary || !out || cap < 16)
        return 0;
    out[0] = '\0';
    if (summary->total_reactions == 0)
        return 0;

    /* "last Nd" — how recent is the latest reaction? */
    char recency[32] = "";
    if (now > 0 && summary->latest_seen > 0 && now >= summary->latest_seen) {
        int64_t days = (now - summary->latest_seen) / 86400;
        if (days <= 0)
            snprintf(recency, sizeof(recency), " (latest today)");
        else
            snprintf(recency, sizeof(recency), " (latest %lldd ago)", (long long)days);
    }

    int n = snprintf(out, cap, "WHAT WORKS: %s — %d positive / %d negative across %d reactions%s.",
                     contact_handle, summary->positive_count, summary->negative_count,
                     summary->total_reactions, recency);
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}
