/* src/eval/register.c — A4 relationship-calibration axis.
 *
 * See include/human/eval/register.h and
 * docs/plans/2026-05-29-humanness-north-star-metric/ for the design.
 *
 * Deterministic, allocation-free heuristics. The signal sets are intentionally
 * small and interpretable: this axis answers "is the register roughly right for
 * this contact?", a coarse calibration question, not a fine stylometric one
 * (that is A1 fidelity's job). Short, overlap-prone tokens ("u", "rn", "ya")
 * use word-boundary matching to avoid the substring-classifier misroute trap
 * (see ~/.claude/rules/substring-classifier-pitfalls.md); distinctive multi-word
 * phrases use plain case-insensitive substring matching.
 */

#include "human/eval/register.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static double clamp01(double x) {
    if (x < 0.0)
        return 0.0;
    if (x > 1.0)
        return 1.0;
    return x;
}

static bool is_blank(const char *text, size_t len) {
    if (!text || len == 0)
        return true;
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)text[i]))
            return false;
    }
    return true;
}

/* Case-insensitive substring search over a bounded buffer. */
static bool contains_ci(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(hay + i, needle, nlen) == 0)
            return true;
    }
    return false;
}

/* Case-insensitive WORD-boundary match — the needle must be flanked by
 * start/end of buffer or a non-alphanumeric char. Prevents "u" matching "but",
 * "rn" matching "turn", "ya" matching "your". */
static bool contains_word_ci(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen)
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

static size_t count_any_word(const char *hay, size_t hlen, const char *const *needles, size_t n) {
    size_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        if (contains_word_ci(hay, hlen, needles[i]))
            hits++;
    }
    return hits;
}

static size_t count_any_sub(const char *hay, size_t hlen, const char *const *needles, size_t n) {
    size_t hits = 0;
    for (size_t i = 0; i < n; i++) {
        if (contains_ci(hay, hlen, needles[i]))
            hits++;
    }
    return hits;
}

static size_t min_sz(size_t a, size_t b) {
    return a < b ? a : b;
}

/* ── formality ─────────────────────────────────────────────────────────── */

double hu_register_formality_estimate(const char *text, size_t len) {
    if (is_blank(text, len))
        return 0.5;

    /* Short, overlap-prone casual tokens — word-boundary matched. */
    static const char *const casual_words[] = {
        "lol",   "lmao",  "omg",   "idk",   "tbh",  "btw", "rn", "u",      "ur",
        "ya",    "yeah",  "yep",   "nah",   "haha", "hey", "yo", "sup",    "dude",
        "kinda", "gonna", "wanna", "gotta", "ngl",  "fr",  "af", "lowkey", "imo",
    };
    /* Distinctive formal phrases — substring matched. */
    static const char *const formal_subs[] = {
        "dear ",
        "sincerely",
        "regards",
        "kindly",
        "please find",
        "i would like",
        "at your convenience",
        "to whom it may",
        "thank you for your",
        "best regards",
        "yours truly",
        "i will respond",
        "in due course",
        "revert to you",
        "regarding this matter",
        "i appreciate your",
    };

    size_t casual_hits =
        count_any_word(text, len, casual_words, sizeof(casual_words) / sizeof(casual_words[0]));
    size_t formal_hits =
        count_any_sub(text, len, formal_subs, sizeof(formal_subs) / sizeof(formal_subs[0]));

    /* All-lowercase alphabetic text is a strong casual tell. */
    bool has_alpha = false, has_upper = false;
    size_t first_alpha = len;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalpha(c)) {
            if (!has_alpha)
                first_alpha = i;
            has_alpha = true;
            if (isupper(c))
                has_upper = true;
        }
    }
    bool all_lower = has_alpha && !has_upper;

    /* Proper sentence shape: capitalized start + terminal punctuation. */
    bool cap_start = (first_alpha < len) && isupper((unsigned char)text[first_alpha]);
    char last = 0;
    for (size_t i = len; i > 0; i--) {
        if (!isspace((unsigned char)text[i - 1])) {
            last = text[i - 1];
            break;
        }
    }
    bool terminal_punct = (last == '.' || last == '!' || last == '?');
    bool proper = cap_start && terminal_punct && casual_hits == 0;

    double score = 0.5;
    score -= 0.18 * (double)min_sz(casual_hits, 3);
    score -= all_lower ? 0.20 : 0.0;
    score += 0.20 * (double)min_sz(formal_hits, 3);
    score += proper ? 0.12 : 0.0;
    return clamp01(score);
}

/* ── warmth ────────────────────────────────────────────────────────────── */

double hu_register_warmth_estimate(const char *text, size_t len) {
    if (is_blank(text, len))
        return 0.5;

    /* Greetings + enthusiasm tokens that read warm even mid-word ("heyyy",
     * "yesss") — substring matched on purpose. */
    static const char *const warm_subs[] = {
        "hey",          "hello",     "miss you", "miss u",  "thinking of you",
        "can't wait",   "cant wait", "so happy", "love it", "love you",
        "amazing",      "congrats",  "xo",       "<3",      "so good to",
        "good to hear", "yay",       "yess",     "buddy",   "babe",
        "sweetie",      "hon",       "cutie",    "hugs",    "talk soon",
        "my friend",    "love,",
    };
    static const char *const warm_words[] = {
        "hi", "yo", "morning", "love", "friend", "pal", "cheers",
    };
    static const char *const distant_subs[] = {
        "received",
        "noted",
        "as discussed",
        "per the",
        "per my",
        "confirmed",
        "processed",
        "transaction",
        "attached",
        "in due course",
        "regarding this matter",
        "revert",
        "follow up shortly",
        "to confirm",
        "has been recorded",
        "as per",
        "for your records",
    };

    size_t warm_hits =
        count_any_sub(text, len, warm_subs, sizeof(warm_subs) / sizeof(warm_subs[0])) +
        count_any_word(text, len, warm_words, sizeof(warm_words) / sizeof(warm_words[0]));
    size_t distant_hits =
        count_any_sub(text, len, distant_subs, sizeof(distant_subs) / sizeof(distant_subs[0]));

    size_t excl = 0;
    bool has_high_byte = false; /* crude emoji / non-ASCII tell */
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '!')
            excl++;
        if ((unsigned char)text[i] >= 0x80)
            has_high_byte = true;
    }

    double score = 0.5;
    score += 0.15 * (double)min_sz(warm_hits, 4);
    score += 0.05 * (double)min_sz(excl, 3);
    score += has_high_byte ? 0.08 : 0.0;
    score -= 0.20 * (double)min_sz(distant_hits, 3);
    return clamp01(score);
}

/* ── A4 composite ──────────────────────────────────────────────────────── */

double hu_relationship_axis_score(const char *text, size_t len, double target_formality,
                                  double target_warmth) {
    double tf = clamp01(target_formality);
    double tw = clamp01(target_warmth);
    double f = hu_register_formality_estimate(text, len);
    double w = hu_register_warmth_estimate(text, len);
    double df = f - tf;
    double dw = w - tw;
    if (df < 0)
        df = -df;
    if (dw < 0)
        dw = -dw;
    double distance = 0.5 * df + 0.5 * dw;
    return clamp01(1.0 - distance);
}
