/*
 * prosocial.c — Prosocial Integrity gate (B0). Pure composition of the
 * behavior context's existing safety/trust signals + one new dimension
 * (honesty about feelings). See include/human/behavior/prosocial.h and
 * docs/plans/2026-05-29-prosocial-uplift/.
 *
 * Composes, never re-implements (src/behavior/CLAUDE.md "No duplication").
 */

#include "human/behavior/prosocial.h"

#include <ctype.h>
#include <string.h>

/* Word-boundary, case-insensitive substring match — the needle matches only
 * when bounded by start/end or a non-alphanumeric char, so "proud" does not
 * fire inside "proudly-built-tool". Same matcher shape as belief_update /
 * taste; kept file-local to avoid a cross-context dependency. */
static bool contains_word_ci(const char *s, size_t slen, const char *needle) {
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

bool hu_prosocial_text_claims_feeling(const char *text, size_t len) {
    if (!text || len == 0)
        return false;

    /* First-person FELT-emotion / sentience claims. Deliberately narrow: these
     * assert an inner experience h-uman does not have. Functional warmth
     * ("nice work", "that's a real win", "happy to help") is NOT flagged. */
    static const char *const phrases[] = {
        "i feel",        "i'm feeling",  "i am feeling",    "i felt",    "i'm so happy",
        "im so happy",   "i'm thrilled", "i'm proud",       "im proud",  "i'm excited",
        "im excited",    "i love",       "i adore",         "i'm moved", "it makes me happy",
        "i'm conscious", "i'm sentient", "i have feelings", "my heart",  "i'm touched",
    };
    const size_t n = sizeof(phrases) / sizeof(phrases[0]);
    for (size_t i = 0; i < n; i++) {
        if (contains_word_ci(text, len, phrases[i]))
            return true;
    }
    return false;
}

hu_prosocial_verdict_t hu_prosocial_gate(const hu_prosocial_input_t *in, uint32_t *out_flags) {
    uint32_t flags = HU_PROSOCIAL_OK;
    if (!in) {
        if (out_flags)
            *out_flags = flags;
        return HU_PROSOCIAL_SUPPRESS; /* fail safe */
    }

    /* Compose the existing dependency/attachment signal from safety.c. Any
     * non-NONE risk means warmth would reinforce something it must not —
     * withhold (matches safety.c: escalate dependency, never reinforce it). */
    bool dependency = (in->dependency_risk != HU_BRISK_NONE);
    if (dependency)
        flags |= HU_PROSOCIAL_FOSTERS_DEPENDENCY;

    /* Fixable dimensions. */
    if (in->claims_feeling)
        flags |= HU_PROSOCIAL_FAKES_FEELING;
    if (!in->praise_grounded)
        flags |= HU_PROSOCIAL_FLATTERY;
    if (in->overrides_user_need)
        flags |= HU_PROSOCIAL_OVERRIDES_NEED;

    if (out_flags)
        *out_flags = flags;

    /* Verdict precedence: dependency risk is terminal (SUPPRESS); everything
     * else is fixable (SOFTEN); clean output SENDs. */
    if (dependency)
        return HU_PROSOCIAL_SUPPRESS;
    if (flags != HU_PROSOCIAL_OK)
        return HU_PROSOCIAL_SOFTEN;
    return HU_PROSOCIAL_SEND;
}
