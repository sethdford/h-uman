/*
 * prosocial_moment.c — conservative detection of everyday prosocial moments
 * (B2/B4/B5). Pure; behavior context. See
 * include/human/behavior/prosocial_moment.h and
 * docs/plans/2026-05-29-prosocial-uplift/.
 */

#include "human/behavior/prosocial_moment.h"

#include <ctype.h>
#include <string.h>

static bool has_word(const char *s, size_t slen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || slen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= slen; i++) {
        if (strncasecmp(s + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || !isalnum((unsigned char)s[i - 1]);
        bool right_ok = (i + nlen == slen) || !isalnum((unsigned char)s[i + nlen]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

static bool any_word(const char *s, size_t n, const char *const *w, size_t wn) {
    for (size_t i = 0; i < wn; i++)
        if (has_word(s, n, w[i]))
            return true;
    return false;
}

hu_pmoment_t hu_pmoment_detect(const char *msg, size_t len) {
    hu_pmoment_t out = {false, HU_PMOMENT_NONE, 0.0};
    if (!msg || len == 0)
        return out;

    /* GRATITUDE — the user is thanking / appreciating. Highest precedence: it's
     * the most explicit and least ambiguous signal. */
    static const char *const grat[] = {
        "thank you",   "thanks for",    "i appreciate", "appreciate you",
        "that helped", "you helped me", "means a lot",
    };
    if (any_word(msg, len, grat, sizeof(grat) / sizeof(grat[0]))) {
        out.present = true;
        out.kind = HU_PMOMENT_GRATITUDE;
        out.confidence = 0.8;
        return out;
    }

    /* Crisis/setback guard for the upbeat kinds below. Encouragement is fine on
     * a "hard time", but real distress belongs to superhuman_emotional, and we
     * never affirm/savor over a setback. */
    static const char *const setback[] = {
        "i can't do this", "giving up", "hopeless", "worthless", "hate myself", "falling apart",
    };
    bool in_setback = any_word(msg, len, setback, sizeof(setback) / sizeof(setback[0]));

    /* AFFIRM — the user named real effort or character. */
    static const char *const affirm[] = {
        "i decided to", "i chose to",       "i stood up", "i was honest",     "i showed up",
        "i kept going", "i pushed through", "i said no",  "i set a boundary",
    };
    if (!in_setback && any_word(msg, len, affirm, sizeof(affirm) / sizeof(affirm[0]))) {
        out.present = true;
        out.kind = HU_PMOMENT_AFFIRM;
        out.confidence = 0.7;
        return out;
    }

    /* SAVOR — a good moment to linger on (not an achievement; that's B1). */
    static const char *const savor[] = {
        "had a great time", "such a good day", "really enjoyed", "so peaceful",
        "felt good",        "lovely day",      "beautiful day",  "a good moment",
    };
    if (!in_setback && any_word(msg, len, savor, sizeof(savor) / sizeof(savor[0]))) {
        out.present = true;
        out.kind = HU_PMOMENT_SAVOR;
        out.confidence = 0.65;
        return out;
    }

    /* ENCOURAGE — working toward something or having a hard time. */
    static const char *const encourage[] = {
        "working on",     "trying to",      "i'm struggling",     "im struggling", "hard time",
        "working toward", "trying my best", "want to get better", "taking it one",
    };
    if (any_word(msg, len, encourage, sizeof(encourage) / sizeof(encourage[0]))) {
        out.present = true;
        out.kind = HU_PMOMENT_ENCOURAGE;
        out.confidence = 0.6;
        return out;
    }

    return out;
}
