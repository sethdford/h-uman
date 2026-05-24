/* src/memory/anticipatory.c
 *
 * Anticipatory memory surfacing. Mirrors src/memory/emotional_context.c
 * in shape — pure read-only scan over personal_model facts, lexicon-
 * filtered, most-recent-wins. The two are deliberately kept as
 * separate modules instead of a shared "contact-fact-surfacer" because
 * their lookback windows + lexicons + prompt prefixes are
 * semantically distinct, and unifying them would require parameters
 * that hurt readability more than the dedup helps.
 *
 * Sprint B Story 7 (docs/plans/2026-05-19-sprint-backlog.md). */

#include "human/memory/anticipatory.h"

#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── lexicon ────────────────────────────────────────────────────────── */
static const char *const k_anticipatory_lexicon[] = {
    "birthday",  "anniversary", "wedding",    "trip",     "vacation", "holiday", "graduation",
    "interview", "exam",        "conference", "deadline", "due",      "surgery", "appointment",
    "moving",    "baby",        "retirement", "flight",   "visiting", "meeting", NULL,
};

/* Word-boundary case-insensitive substring match. Same shape as the
 * emotional_context helper; intentionally duplicated rather than
 * factored to keep each module's lexicon discipline self-contained. */
bool hu_anticipatory_lexicon_word_match(const char *haystack, const char *needle) {
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

static bool fact_matches_lexicon(const hu_heuristic_fact_t *f) {
    for (size_t i = 0; k_anticipatory_lexicon[i]; i++) {
        const char *kw = k_anticipatory_lexicon[i];
        if (hu_anticipatory_lexicon_word_match(f->subject, kw))
            return true;
        if (hu_anticipatory_lexicon_word_match(f->predicate, kw))
            return true;
        if (hu_anticipatory_lexicon_word_match(f->object, kw))
            return true;
    }
    return false;
}

static bool contact_matches(const char *fact_handle, const char *query_handle) {
    if (!fact_handle || !query_handle || !fact_handle[0] || !query_handle[0])
        return false;
    return strncasecmp(fact_handle, query_handle, HU_PROV_HANDLE_MAX) == 0;
}

size_t hu_anticipatory_for_contact(const struct hu_personal_model *model_opaque,
                                   const char *contact_handle, int64_t now,
                                   int64_t lookback_seconds, char *out, size_t cap) {
    if (!model_opaque || !contact_handle || !*contact_handle || !out || cap == 0)
        return 0;
    out[0] = '\0';
    const hu_personal_model_t *model = (const hu_personal_model_t *)model_opaque;
    if (model->fact_count == 0)
        return 0;

    if (lookback_seconds <= 0)
        lookback_seconds = HU_ANTICIPATORY_LOOKBACK_DEFAULT_SEC;

    if (now == 0) {
#if HU_IS_TEST
        return 0;
#else
        now = (int64_t)time(NULL);
#endif
    }
    int64_t window_start = now - lookback_seconds;

    const hu_heuristic_fact_t *best = NULL;
    int64_t best_ts = 0;
    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];
        if (!contact_matches(f->provenance.contact_handle, contact_handle))
            continue;
        if (f->last_seen_at == 0 || f->last_seen_at < window_start)
            continue;
        float eff = hu_heuristic_fact_effective_confidence(f, now);
        if (eff < HU_ANTICIPATORY_MIN_CONFIDENCE)
            continue;
        if (!fact_matches_lexicon(f))
            continue;
        if (f->last_seen_at > best_ts) {
            best = f;
            best_ts = f->last_seen_at;
        }
    }

    if (!best)
        return 0;

    int n =
        snprintf(out, cap, "UPCOMING: %s mentioned: %s %s %s.", contact_handle,
                 best->subject[0] ? best->subject : "they",
                 best->predicate[0] ? best->predicate : "has", best->object[0] ? best->object : "");
    if (n < 0)
        return 0;
    return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
}
