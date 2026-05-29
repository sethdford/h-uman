/*
 * win_detect.c — conservative win detection (B1a). Pure; behavior context.
 * See include/human/behavior/win_detect.h and
 * docs/plans/2026-05-29-prosocial-uplift/.
 */

#include "human/behavior/win_detect.h"

#include <ctype.h>
#include <string.h>

/* Word-boundary, case-insensitive match (needle may contain spaces; only the
 * outer edges are boundary-checked). Same matcher shape as the rest of the
 * prosocial/aliveness series. */
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

static bool any_word(const char *s, size_t slen, const char *const *needles, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (has_word(s, slen, needles[i]))
            return true;
    return false;
}

hu_win_signal_t hu_win_detect(const char *msg, size_t len) {
    hu_win_signal_t out = {false, HU_WIN_NONE, 0.0};
    if (!msg || len == 0)
        return out;

    /* Negation / setback guard — if present, refuse to celebrate. Cheap and
     * conservative: one negator anywhere vetoes the whole message. */
    static const char *const negators[] = {
        "didn't",   "did not", "not ",         "no longer", "couldn't",  "could not",
        "failed",   "won't",   "wasn't",       "isn't",     "can't",     "unfortunately",
        "rejected", "denied",  "fell through", "lost",      "cancelled", "canceled",
    };
    if (any_word(msg, len, negators, sizeof(negators) / sizeof(negators[0])))
        return out;

    static const char *const achievement[] = {
        "i did it",     "i finished",  "i shipped", "i passed",   "i got the job", "i landed",
        "i nailed",     "i completed", "i won",     "we shipped", "we launched",   "got promoted",
        "got accepted", "got in",      "aced",      "graduated",
    };
    static const char *const milestone[] = {
        "anniversary", "first time", "years together", "years sober", "milestone",
        "100th",       "1000th",     "one year",       "ten years",   "big day",
    };
    static const char *const good_news[] = {
        "great news",   "good news",      "excited to share", "guess what",
        "amazing news", "wonderful news", "so excited",       "thrilled to share",
    };

    if (any_word(msg, len, achievement, sizeof(achievement) / sizeof(achievement[0]))) {
        out.is_win = true;
        out.kind = HU_WIN_ACHIEVEMENT;
        out.confidence = 0.85;
    } else if (any_word(msg, len, milestone, sizeof(milestone) / sizeof(milestone[0]))) {
        out.is_win = true;
        out.kind = HU_WIN_MILESTONE;
        out.confidence = 0.7;
    } else if (any_word(msg, len, good_news, sizeof(good_news) / sizeof(good_news[0]))) {
        out.is_win = true;
        out.kind = HU_WIN_GOOD_NEWS;
        out.confidence = 0.75;
    }
    return out;
}
