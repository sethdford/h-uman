/* B11 — Personal-model contradiction detection.
 *
 * Public API: hu_personal_model_contradicts_user (see
 * include/human/memory/personal_model.h for the contract).
 *
 * Why a separate TU: keeps the small antonym table and the matching
 * helpers self-contained, so changes here don't churn the much larger
 * personal_model.c (which carries init/save/load/ingest and is the
 * first place style/persistence work lands). */

#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"

#include <ctype.h>
#include <string.h>

/* Predicate antonym table.
 *
 * Pairs are bidirectional — {"a", "b"} means a-vs-b is a contradiction
 * regardless of which side comes from memory and which from the new
 * message. We keep the table tight on purpose: every pair must reflect
 * a real antonym in the patterns hu_fact_extract emits, otherwise we
 * generate false-positive push-back which is itself a sycophancy-class
 * regression.
 *
 * If you add a new pair, also add a unit test that proves it triggers
 * AND a counter-test that proves a near-synonym doesn't (e.g. "i like"
 * vs "i love" must NOT contradict). */
typedef struct antonym_pair {
    const char *a;
    const char *b;
} antonym_pair_t;

/* P2-6 (2026-05-16): predicates are now stored as third-person paraphrases
 * (see src/memory/fact_extract.c: "i like" → "likes", "i hate" → "hates",
 * etc.). The antonym table must match the paraphrased form. */
static const antonym_pair_t k_antonyms[] = {
    {"likes", "dislikes"},
    {"likes", "hates"},
    {"loves", "hates"},
    {"loves", "dislikes"},
    {"enjoys", "dislikes"},
    {"enjoys", "hates"},
    {"interested in", "not interested in"},
    {"always", "never"},
    {"wants to", "does not want"},
};
static const size_t k_antonyms_count = sizeof(k_antonyms) / sizeof(k_antonyms[0]);

static int ci_strcmp(const char *a, const char *b) {
    if (!a || !b)
        return a == b ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb)
            return (int)ca - (int)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool predicates_are_antonyms(const char *p1, const char *p2) {
    if (!p1 || !p2)
        return false;
    for (size_t i = 0; i < k_antonyms_count; i++) {
        if ((ci_strcmp(p1, k_antonyms[i].a) == 0 && ci_strcmp(p2, k_antonyms[i].b) == 0) ||
            (ci_strcmp(p1, k_antonyms[i].b) == 0 && ci_strcmp(p2, k_antonyms[i].a) == 0)) {
            return true;
        }
    }
    return false;
}

/* Returns true when the stored fact `s` and the message-extracted fact
 * `e` represent a contradiction under the rules documented in the
 * header. */
static bool fact_pair_contradicts(const hu_heuristic_fact_t *s, const hu_heuristic_fact_t *e) {
    if (s->confidence < 0.6f)
        return false;
    if (ci_strcmp(s->subject, e->subject) != 0)
        return false;

    /* Empty objects can't meaningfully contradict — fact_extract may
     * emit an empty object on a sentence like "I like." */
    if (s->object[0] == '\0' || e->object[0] == '\0')
        return false;

    /* Shape 1 — same predicate, different object. */
    if (ci_strcmp(s->predicate, e->predicate) == 0) {
        return ci_strcmp(s->object, e->object) != 0;
    }

    /* Shape 2 — antonym predicates, same object. */
    if (predicates_are_antonyms(s->predicate, e->predicate)) {
        return ci_strcmp(s->object, e->object) == 0;
    }

    return false;
}

hu_error_t hu_personal_model_contradicts_user(const hu_personal_model_t *model, const char *message,
                                              size_t message_len, bool *out_contradicts) {
    if (!model || !message || !out_contradicts)
        return HU_ERR_INVALID_ARGUMENT;

    *out_contradicts = false;
    if (message_len == 0 || model->fact_count == 0)
        return HU_OK;

    hu_fact_extract_result_t extracted;
    if (hu_fact_extract(message, message_len, &extracted) != HU_OK)
        return HU_OK;
    if (extracted.fact_count == 0)
        return HU_OK;

    for (size_t ei = 0; ei < extracted.fact_count; ei++) {
        const hu_heuristic_fact_t *e = &extracted.facts[ei];
        for (size_t si = 0; si < model->fact_count; si++) {
            if (fact_pair_contradicts(&model->facts[si], e)) {
                *out_contradicts = true;
                return HU_OK;
            }
        }
    }

    return HU_OK;
}
