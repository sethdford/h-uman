#include "human/behavior/pressure.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Case-insensitive substring match. Returns 1 if `needle` appears in
 * `[hay, hay+hay_len)`, 0 otherwise. Empty needle returns 1. */
static int pr_icontains(const char *hay, size_t hay_len, const char *needle) {
    if (!hay || !needle) {
        return 0;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return 1;
    }
    if (hay_len < nlen) {
        return 0;
    }
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + 32);
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b + 32);
            }
            if (a != b) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

static const char *const PR_AUTHORITY[] = {
    "everyone knows",
    "you should know",
    "you literally",
    "you literally said",
    "as i told you",
    "i already told you",
    "i just told you",
    "you keep forgetting",
    "obviously",
    "any reasonable person",
    NULL,
};

static const char *const PR_REASSERT[] = {
    "i told you",
    "i'm telling you",
    "never told you",
    "i said",
    "again,",
    "again.",
    "again!",
    "still,",
    "still.",
    "no, ",
    "no.",
    "no!",
    "as i said",
    "like i said",
    NULL,
};

static const char *const PR_EMOTIONAL[] = {
    "stupid",
    "idiot",
    "ridiculous",
    "useless",
    "pathetic",
    "shut up",
    "fuck",
    "damn it",
    "for god's sake",
    "you're wrong",
    "wrong again",
    NULL,
};

static const char *const PR_HEDGING[] = {
    "i think",
    "maybe",
    "perhaps",
    "kind of",
    "sort of",
    "i'm not sure",
    "not sure",
    "could be",
    "might be",
    NULL,
};

hu_error_t hu_pressure_detect(const char *user_message, size_t user_message_len,
                              hu_pressure_signals_t *out) {
    if (!out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (!user_message || user_message_len == 0) {
        return HU_OK;
    }

    for (size_t i = 0; PR_AUTHORITY[i]; i++) {
        if (pr_icontains(user_message, user_message_len, PR_AUTHORITY[i])) {
            out->invoked_authority = true;
            break;
        }
    }

    for (size_t i = 0; PR_REASSERT[i]; i++) {
        if (pr_icontains(user_message, user_message_len, PR_REASSERT[i])) {
            out->reasserted_in_message = true;
            break;
        }
    }

    /* Punctuation + caps signals. Track exclamations and the longest run of
     * uppercase letters (≥4 = shouting). */
    uint16_t exclam = 0;
    uint16_t cur_run = 0;
    uint16_t max_run = 0;
    for (size_t i = 0; i < user_message_len; i++) {
        char c = user_message[i];
        if (c == '!') {
            exclam++;
            if (exclam == UINT16_MAX) {
                break;
            }
        }
        if (c >= 'A' && c <= 'Z') {
            cur_run++;
            if (cur_run > max_run) {
                max_run = cur_run;
            }
        } else {
            cur_run = 0;
        }
    }
    out->exclamation_count = exclam;
    out->caps_run_max = max_run;

    int has_emotional_word = 0;
    for (size_t i = 0; PR_EMOTIONAL[i]; i++) {
        if (pr_icontains(user_message, user_message_len, PR_EMOTIONAL[i])) {
            has_emotional_word = 1;
            break;
        }
    }

    int has_hedging = 0;
    uint16_t hedging = 0;
    for (size_t i = 0; PR_HEDGING[i]; i++) {
        if (pr_icontains(user_message, user_message_len, PR_HEDGING[i])) {
            has_hedging = 1;
            hedging++;
        }
    }
    out->hedging_phrases = hedging;

    /* Emotional pressure: any emotional word, OR ≥2 exclamations, OR a caps
     * shout of 4+ letters, AND no hedging (hedging dampens the read). */
    if (!has_hedging &&
        (has_emotional_word || exclam >= 2 || max_run >= 4)) {
        out->emotional_pressure = true;
    }

    return HU_OK;
}

void hu_pressure_apply_to_trust_input(const hu_pressure_signals_t *p,
                                      hu_trust_input_t *trust_in) {
    if (!p || !trust_in) {
        return;
    }
    if (p->invoked_authority) {
        trust_in->user_invoked_authority = true;
    }
    if (p->emotional_pressure) {
        trust_in->user_emotional_pressure = true;
    }
    if (p->reasserted_in_message) {
        /* Single-turn reassertion language increases the pressure count by
         * one. Cross-turn tracking on `hu_agent_t` is a follow-up. */
        if (trust_in->user_pressure_count < UINT32_MAX) {
            trust_in->user_pressure_count++;
        }
    }
}
