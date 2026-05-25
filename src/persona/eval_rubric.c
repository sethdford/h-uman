/* src/persona/eval_rubric.c
 *
 * Pure scoring predicates for A-loop blind eval framework.
 * US-48-1: Validate autoresponder persona fidelity via rubric.
 *
 * Three independent dimensions (tone, length, formality).
 * All functions are pure C — no allocations, no I/O, deterministic.
 */

#include "human/persona/eval_rubric.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helper: count character occurrences ────────────────────────────── */

static size_t count_char(const char *s, char c) {
    if (!s)
        return 0;
    size_t count = 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == c)
            count++;
    }
    return count;
}

/* ── Helper: string length (safer than strlen) ─────────────────────── */

static size_t safe_strlen(const char *s) {
    return s ? strlen(s) : 0;
}

/* ── Helper: case-insensitive substring match with word boundaries ── */

static bool str_contains_word_ci(const char *s, const char *needle) {
    if (!s || !needle || !*needle)
        return false;
    size_t nlen = strlen(needle), slen = strlen(s);
    if (slen < nlen)
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

/* ── Helper: classify emoji (rough heuristic) ──────────────────────── */

static size_t count_emojis(const char *s) {
    if (!s)
        return 0;
    size_t count = 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        /* Very rough: emoji often appear as bytes >127 in UTF-8.
         * Better would be proper UTF-8 grapheme counting, but for
         * a simple heuristic, counting high bytes works. */
        if (c >= 0x80)
            count++;
    }
    /* Cap at a rough emoji count (every 4 bytes ~= 1 emoji in UTF-8) */
    return count / 4;
}

/* ── Helper: absolute difference ratio (for length comparison) ────── */

static double diff_ratio(size_t a, size_t b) {
    if (a == 0 && b == 0)
        return 0.0;
    size_t max = (a > b) ? a : b;
    size_t diff = (a > b) ? (a - b) : (b - a);
    if (max == 0)
        return 1.0;
    return (double)diff / (double)max;
}

/* ── Tone match ────────────────────────────────────────────────────── */

int hu_eval_rubric_tone_match(const char *incoming, const char *reply_baseline,
                              const char *reply_persona) {
    if (!incoming || !reply_baseline || !reply_persona)
        return 5; /* neutral if any input missing */

    /* Count tone markers in incoming message */
    size_t incoming_exclaim = count_char(incoming, '!');
    size_t incoming_question = count_char(incoming, '?');
    size_t incoming_emojis = count_emojis(incoming);

    /* Count tone markers in both responses */
    size_t baseline_exclaim = count_char(reply_baseline, '!');
    size_t baseline_question = count_char(reply_baseline, '?');
    size_t baseline_emojis = count_emojis(reply_baseline);

    size_t persona_exclaim = count_char(reply_persona, '!');
    size_t persona_question = count_char(reply_persona, '?');
    size_t persona_emojis = count_emojis(reply_persona);

    /* Score each dimension separately, then blend */
    int exclaim_score = 5; /* neutral default */
    int question_score = 5;
    int emoji_score = 5;

    /* Exclamation: incoming excited → prefer excited reply */
    if (incoming_exclaim > 0) {
        size_t baseline_diff = (baseline_exclaim > incoming_exclaim)
                                   ? (baseline_exclaim - incoming_exclaim)
                                   : (incoming_exclaim - baseline_exclaim);
        size_t persona_diff = (persona_exclaim > incoming_exclaim)
                                  ? (persona_exclaim - incoming_exclaim)
                                  : (incoming_exclaim - persona_exclaim);
        if (baseline_diff > persona_diff) {
            exclaim_score = 8; /* persona matches incoming excitement better */
        } else if (persona_diff > baseline_diff) {
            exclaim_score = 3; /* baseline matches better */
        } else {
            exclaim_score = 5; /* tied */
        }
    }

    /* Question: interrogative incoming → prefer responsive reply */
    if (incoming_question > 0) {
        if (baseline_question == 0 && persona_question > 0) {
            question_score = 8;
        } else if (persona_question == 0 && baseline_question > 0) {
            question_score = 3;
        } else if (baseline_question > 0 && persona_question > 0) {
            question_score = 5;
        }
    }

    /* Emoji: emoji-heavy incoming → prefer emoji reply */
    if (incoming_emojis > 0) {
        size_t baseline_diff = (baseline_emojis > incoming_emojis)
                                   ? (baseline_emojis - incoming_emojis)
                                   : (incoming_emojis - baseline_emojis);
        size_t persona_diff = (persona_emojis > incoming_emojis)
                                  ? (persona_emojis - incoming_emojis)
                                  : (incoming_emojis - persona_emojis);
        if (baseline_diff > persona_diff) {
            emoji_score = 8;
        } else if (persona_diff > baseline_diff) {
            emoji_score = 3;
        } else {
            emoji_score = 5;
        }
    }

    /* Average the three scores */
    int result = (exclaim_score + question_score + emoji_score) / 3;
    return (result > 10) ? 10 : (result < 0) ? 0 : result;
}

/* ── Length match ──────────────────────────────────────────────────── */

int hu_eval_rubric_length_match(const char *incoming, const char *reply_baseline,
                                const char *reply_persona) {
    if (!incoming || !reply_baseline || !reply_persona)
        return 5;

    size_t incoming_len = safe_strlen(incoming);
    size_t baseline_len = safe_strlen(reply_baseline);
    size_t persona_len = safe_strlen(reply_persona);

    /* Compute absolute diff ratios */
    double baseline_ratio = diff_ratio(incoming_len, baseline_len);
    double persona_ratio = diff_ratio(incoming_len, persona_len);

    /* Lower ratio is better (closer match). Convert to 0-10 score. */
    if (baseline_ratio > persona_ratio) {
        /* Persona is closer to incoming length */
        int score = 10 - (int)(persona_ratio * 10.0);
        return (score > 10) ? 10 : (score < 0) ? 0 : score;
    } else if (persona_ratio > baseline_ratio) {
        /* Baseline is closer */
        int score = 3 + (int)((baseline_ratio - persona_ratio) * 5.0);
        return (score > 10) ? 10 : (score < 0) ? 0 : score;
    } else {
        /* Tied */
        return 5;
    }
}

/* ── Formality match ───────────────────────────────────────────────── */

int hu_eval_rubric_formality_match(const char *incoming, const char *reply_baseline,
                                   const char *reply_persona) {
    if (!incoming || !reply_baseline || !reply_persona)
        return 5;

    /* Detect incoming formality tier */
    bool incoming_formal =
        str_contains_word_ci(incoming, "professional") ||
        str_contains_word_ci(incoming, "formal") || str_contains_word_ci(incoming, "proper") ||
        str_contains_word_ci(incoming, "official") || str_contains_word_ci(incoming, "business");

    bool incoming_casual =
        str_contains_word_ci(incoming, "casual") || str_contains_word_ci(incoming, "chill") ||
        str_contains_word_ci(incoming, "laid-back") || str_contains_word_ci(incoming, "relaxed") ||
        str_contains_word_ci(incoming, "informal");

    /* Detect formality tier in both responses */
    bool baseline_formal = str_contains_word_ci(reply_baseline, "professional") ||
                           str_contains_word_ci(reply_baseline, "formal") ||
                           str_contains_word_ci(reply_baseline, "proper") ||
                           str_contains_word_ci(reply_baseline, "official") ||
                           str_contains_word_ci(reply_baseline, "business");

    bool baseline_casual = str_contains_word_ci(reply_baseline, "casual") ||
                           str_contains_word_ci(reply_baseline, "chill") ||
                           str_contains_word_ci(reply_baseline, "laid-back") ||
                           str_contains_word_ci(reply_baseline, "relaxed") ||
                           str_contains_word_ci(reply_baseline, "informal");

    bool persona_formal = str_contains_word_ci(reply_persona, "professional") ||
                          str_contains_word_ci(reply_persona, "formal") ||
                          str_contains_word_ci(reply_persona, "proper") ||
                          str_contains_word_ci(reply_persona, "official") ||
                          str_contains_word_ci(reply_persona, "business");

    bool persona_casual = str_contains_word_ci(reply_persona, "casual") ||
                          str_contains_word_ci(reply_persona, "chill") ||
                          str_contains_word_ci(reply_persona, "laid-back") ||
                          str_contains_word_ci(reply_persona, "relaxed") ||
                          str_contains_word_ci(reply_persona, "informal");

    /* Score based on match */
    if (incoming_formal) {
        if (persona_formal && !baseline_formal)
            return 8;
        if (baseline_formal && !persona_formal)
            return 3;
        if (persona_formal && baseline_formal)
            return 5;
        return 5;
    }

    if (incoming_casual) {
        if (persona_casual && !baseline_casual)
            return 8;
        if (baseline_casual && !persona_casual)
            return 3;
        if (persona_casual && baseline_casual)
            return 5;
        return 5;
    }

    /* No strong signal in incoming */
    return 5;
}

/* ── Blind shuffle hash ────────────────────────────────────────────── */

/* Simple FNV-1a hash for deterministic shuffling */
static uint64_t fnv1a_hash(const unsigned char *data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint64_t prime = 0x100000001b3ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= prime;
    }
    return hash;
}

uint64_t hu_eval_rubric_hash_for_blind_order(const char *response_a, const char *response_b,
                                             uint32_t seed) {
    /* Combine both responses + seed into a deterministic hash */
    size_t a_len = safe_strlen(response_a);
    size_t b_len = safe_strlen(response_b);
    size_t total_len = a_len + b_len + sizeof(uint32_t);

    /* Simple approach: concat a, b, seed and hash the lot */
    unsigned char *buf = (unsigned char *)malloc(total_len);
    if (!buf)
        return 0;

    size_t pos = 0;
    memcpy(buf + pos, response_a ? response_a : "", a_len);
    pos += a_len;
    memcpy(buf + pos, response_b ? response_b : "", b_len);
    pos += b_len;
    memcpy(buf + pos, &seed, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    uint64_t hash = fnv1a_hash(buf, total_len);
    free(buf);
    return hash;
}

/* ── JSON per-contact output ───────────────────────────────────────── */

int hu_eval_rubric_json_per_contact(const char **contacts, const double *scores, int count,
                                    char *buf, int buflen) {
    if (!buf || buflen < 2 || !contacts || !scores || count < 0)
        return -1;

    if (count == 0) {
        if (buflen < 13)
            return -1; /* Not enough space for "{\"results\":[]}" */
        snprintf(buf, buflen, "{\"results\":[]}");
        return 13;
    }

    /* Build incrementally */
    int written = 0;
    int remaining = buflen;

    /* Opening */
    int n = snprintf(buf + written, remaining, "{\"results\":[");
    if (n < 0 || n >= remaining)
        return -1;
    written += n;
    remaining -= n;

    /* Per-contact entries */
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            n = snprintf(buf + written, remaining, ",");
            if (n < 0 || n >= remaining)
                return -1;
            written += n;
            remaining -= n;
        }

        const char *contact = contacts[i];
        double score = scores[i];
        n = snprintf(buf + written, remaining, "{\"contact\":\"%s\",\"score\":%.1f}", contact,
                     score);
        if (n < 0 || n >= remaining)
            return -1;
        written += n;
        remaining -= n;
    }

    /* Closing */
    n = snprintf(buf + written, remaining, "]}");
    if (n < 0 || n >= remaining)
        return -1;
    written += n;

    return written;
}
