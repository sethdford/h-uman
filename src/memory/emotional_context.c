/* src/memory/emotional_context.c
 *
 * Cross-conversation emotional memory. See header for contract +
 * rationale. Pure read-only scan over the personal model.
 *
 * Sprint B Story 2 (docs/plans/2026-05-19-sprint-backlog.md). */

#include "human/memory/emotional_context.h"

#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── lexicon ───────────────────────────────────────────────────────────
 *
 * English-only "tender emotional event" keywords. The match function
 * (hu_emotional_context_lexicon_word_match) is word-boundary aware,
 * so we don't need to enumerate inflections like "sickness" or
 * "grieving" — adding a stem here costs the user-facing risk of
 * false-positives ("grieving for our team's loss"), so we keep the
 * list tight.
 *
 * Adding new entries is the extension point — no API change needed.
 * Future i18n: callers wanting non-English locales should provide
 * their own match function rather than mutating this list. */
static const char *const k_tender_lexicon[] = {
    "sick",      "ill", /* "she's ill" */
    "lost",      "dying",      "died",        "death",     "divorce",  "separated", /* relationship
                                                                                     */
    "funeral",   "grief",      "grieving",    "mourning",  "worried",  "scared",    "anxious",
    "depressed", "depression", "miscarriage", "diagnosed", "hospital", "hospice",   "cancer",
    NULL, /* sentinel */
};

/* ── word-boundary match (public for unit testing) ─────────────────── */

bool hu_emotional_context_lexicon_word_match(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle)
        return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (hlen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || !isalnum((unsigned char)haystack[i - 1]);
        bool right_ok = (i + nlen == hlen) || !isalnum((unsigned char)haystack[i + nlen]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

/* True when ANY field of `f` (subject/predicate/object) contains a
 * lexicon keyword at word boundaries. Iterates the lexicon array at
 * each call — N is small (< 25), O(N×M) is fine here. */
static bool fact_matches_lexicon(const hu_heuristic_fact_t *f) {
    for (size_t i = 0; k_tender_lexicon[i]; i++) {
        const char *kw = k_tender_lexicon[i];
        if (hu_emotional_context_lexicon_word_match(f->subject, kw))
            return true;
        if (hu_emotional_context_lexicon_word_match(f->predicate, kw))
            return true;
        if (hu_emotional_context_lexicon_word_match(f->object, kw))
            return true;
    }
    return false;
}

/* Case-insensitive equality on contact-handle slot (length-bounded to
 * the provenance contact_handle cap to avoid running past the field). */
static bool contact_matches(const char *fact_handle, const char *query_handle) {
    if (!fact_handle || !query_handle)
        return false;
    if (!fact_handle[0] || !query_handle[0])
        return false;
    return strncasecmp(fact_handle, query_handle, HU_PROV_HANDLE_MAX) == 0;
}

/* ── main entry ────────────────────────────────────────────────────── */

size_t hu_emotional_context_for_contact(const struct hu_personal_model *model_opaque,
                                        const char *contact_handle, int64_t now,
                                        int64_t lookback_seconds, char *out, size_t cap) {
    if (!model_opaque || !contact_handle || !*contact_handle || !out || cap == 0)
        return 0;
    out[0] = '\0';

    const hu_personal_model_t *model = (const hu_personal_model_t *)model_opaque;
    if (model->fact_count == 0)
        return 0;

    if (lookback_seconds <= 0)
        lookback_seconds = HU_EMOTIONAL_CONTEXT_LOOKBACK_DEFAULT_SEC;

    if (now == 0) {
#if HU_IS_TEST
        /* Deterministic in tests: callers must pass a real `now` to
         * exercise the lookback gate. Falling through to time(NULL)
         * here would make test outputs flaky. */
        return 0;
#else
        now = (int64_t)time(NULL);
#endif
    }
    int64_t window_start = now - lookback_seconds;

    /* Find the most recent matching fact. Most-recent because:
     *   1. Resurfacing OLD grief feels intrusive.
     *   2. The backlog spec says "most recent tender fact only" —
     *      no multi-event aggregation. */
    const hu_heuristic_fact_t *best = NULL;
    int64_t best_ts = 0;
    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];

        /* Contact match (case-insensitive). Identity-resolver should
         * have canonicalized BEFORE ingest, so cross-channel reactions
         * from Alice@imessage + Alice@slack land under one handle. */
        if (!contact_matches(f->provenance.contact_handle, contact_handle))
            continue;

        /* Lookback window. last_seen_at == 0 means "never refreshed";
         * skip — we want recent events, not stale-by-construction. */
        if (f->last_seen_at == 0 || f->last_seen_at < window_start)
            continue;

        /* Confidence floor (post-decay). */
        float eff = hu_heuristic_fact_effective_confidence(f, now);
        if (eff < HU_EMOTIONAL_CONTEXT_MIN_CONFIDENCE)
            continue;

        /* Lexicon match LAST — cheapest filters first. */
        if (!fact_matches_lexicon(f))
            continue;

        if (f->last_seen_at > best_ts) {
            best = f;
            best_ts = f->last_seen_at;
        }
    }

    if (!best)
        return 0;

    /* Render: prefix + subject-verb-object reconstruction. We DON'T
     * tell the agent what tone to use — that's the persona's job. We
     * just surface the fact. */
    int n =
        snprintf(out, cap, "EMOTIONAL CONTEXT: %s recently mentioned: %s %s %s.", contact_handle,
                 best->subject[0] ? best->subject : "they",
                 best->predicate[0] ? best->predicate : "is", best->object[0] ? best->object : "");
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}
