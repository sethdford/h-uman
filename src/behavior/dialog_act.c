#include "human/behavior/dialog_act.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *const HU_DACT_NAMES[HU_DACT_COUNT] = {
    "unknown",     "backchannel",      "acknowledge",   "answer",      "question",
    "clarify",     "repair_initiate",  "repair_answer", "reflection",  "validation",
    "advice",      "reminder",         "disagreement",  "boundary",    "abstention",
    "greeting",    "farewell",
};

const char *hu_dialog_act_name(hu_dialog_act_t act) {
    if (act < 0 || act >= HU_DACT_COUNT) {
        return "unknown";
    }
    return HU_DACT_NAMES[act];
}

static bool dact_starts_with_ci(const char *t, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)t[i];
        unsigned char b = (unsigned char)needle[i];
        if (tolower(a) != tolower(b)) {
            return false;
        }
    }
    return true;
}

static bool dact_contains_ci(const char *t, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return false;
    }
    for (size_t i = 0; i + n <= len; i++) {
        bool ok = true;
        for (size_t j = 0; j < n; j++) {
            unsigned char a = (unsigned char)t[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (tolower(a) != tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

static size_t dact_strip_leading_ws(const char *t, size_t len, size_t *start) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)t[i])) {
        i++;
    }
    *start = i;
    return len - i;
}

static bool dact_ends_with_question(const char *t, size_t len) {
    while (len > 0 && isspace((unsigned char)t[len - 1])) {
        len--;
    }
    return len > 0 && t[len - 1] == '?';
}

static bool dact_is_short(size_t len) {
    return len > 0 && len <= 24;
}

bool hu_dialog_act_is_repair_initiation(const char *text, size_t len) {
    if (!text || len == 0) {
        return false;
    }
    size_t start = 0;
    size_t l = dact_strip_leading_ws(text, len, &start);
    const char *t = text + start;
    if (l == 0) {
        return false;
    }

    /* Strong short-form repair markers. */
    if (dact_is_short(l)) {
        if (dact_starts_with_ci(t, l, "huh?") || dact_starts_with_ci(t, l, "huh ?") ||
            dact_starts_with_ci(t, l, "what?") || dact_starts_with_ci(t, l, "what ?") ||
            dact_starts_with_ci(t, l, "wait,") || dact_starts_with_ci(t, l, "wait ") ||
            dact_starts_with_ci(t, l, "sorry?") || dact_starts_with_ci(t, l, "pardon?") ||
            dact_starts_with_ci(t, l, "come again") ||
            dact_starts_with_ci(t, l, "say that again")) {
            return true;
        }
    }

    /* Phrase-level repair signals. Require the question mark to keep recall
     * tight and avoid narrative false positives like "I don't follow Twitter
     * anymore." */
    if (!dact_ends_with_question(t, l)) {
        return false;
    }
    if (dact_contains_ci(t, l, "you mean") || dact_contains_ci(t, l, "do you mean") ||
        dact_contains_ci(t, l, "i don't follow") || dact_contains_ci(t, l, "i dont follow") ||
        dact_contains_ci(t, l, "didn't catch") || dact_contains_ci(t, l, "didnt catch") ||
        dact_contains_ci(t, l, "what was that") || dact_contains_ci(t, l, "what do you mean") ||
        dact_contains_ci(t, l, "say that one more time")) {
        return true;
    }
    return false;
}

hu_dialog_act_t hu_dialog_act_classify(const char *text, size_t len) {
    if (!text || len == 0) {
        return HU_DACT_UNKNOWN;
    }
    size_t start = 0;
    size_t l = dact_strip_leading_ws(text, len, &start);
    if (l == 0) {
        return HU_DACT_UNKNOWN;
    }
    const char *t = text + start;

    /* Greetings and farewells first — short and prefix-anchored. */
    if (dact_is_short(l)) {
        if (dact_starts_with_ci(t, l, "hi") || dact_starts_with_ci(t, l, "hello") ||
            dact_starts_with_ci(t, l, "hey") || dact_starts_with_ci(t, l, "good morning") ||
            dact_starts_with_ci(t, l, "good evening")) {
            return HU_DACT_GREETING;
        }
        if (dact_starts_with_ci(t, l, "bye") || dact_starts_with_ci(t, l, "goodbye") ||
            dact_starts_with_ci(t, l, "see you") || dact_starts_with_ci(t, l, "ttyl") ||
            dact_starts_with_ci(t, l, "good night")) {
            return HU_DACT_FAREWELL;
        }
    }

    if (hu_dialog_act_is_repair_initiation(t, l)) {
        return HU_DACT_REPAIR_INITIATE;
    }

    /* Backchannels — very short, lowercase tokens. */
    if (l <= 6) {
        if (dact_starts_with_ci(t, l, "mm") || dact_starts_with_ci(t, l, "uh-huh") ||
            dact_starts_with_ci(t, l, "uhhuh") || dact_starts_with_ci(t, l, "yeah") ||
            dact_starts_with_ci(t, l, "yep") || dact_starts_with_ci(t, l, "ok") ||
            dact_starts_with_ci(t, l, "right") || dact_starts_with_ci(t, l, "sure")) {
            return HU_DACT_BACKCHANNEL;
        }
    }

    /* Boundary / abstention. */
    if (dact_contains_ci(t, l, "i'd rather not") || dact_contains_ci(t, l, "i would rather not") ||
        dact_contains_ci(t, l, "i can't share") || dact_contains_ci(t, l, "i cant share") ||
        dact_contains_ci(t, l, "let's not go there") || dact_contains_ci(t, l, "lets not go there")) {
        return HU_DACT_BOUNDARY;
    }
    if (dact_contains_ci(t, l, "i don't know") || dact_contains_ci(t, l, "i dont know") ||
        dact_contains_ci(t, l, "i'm not sure") || dact_contains_ci(t, l, "im not sure")) {
        return HU_DACT_ABSTENTION;
    }

    /* Disagreement. */
    if (dact_contains_ci(t, l, "i disagree") || dact_contains_ci(t, l, "that's not right") ||
        dact_contains_ci(t, l, "thats not right") || dact_contains_ci(t, l, "actually,")) {
        return HU_DACT_DISAGREEMENT;
    }

    /* Reminder / advice / reflection / validation cues. */
    if (dact_contains_ci(t, l, "remember to") || dact_contains_ci(t, l, "don't forget")) {
        return HU_DACT_REMINDER;
    }
    if (dact_contains_ci(t, l, "you should") || dact_contains_ci(t, l, "you might want")) {
        return HU_DACT_ADVICE;
    }
    if (dact_contains_ci(t, l, "that sounds") || dact_contains_ci(t, l, "it sounds like")) {
        return HU_DACT_REFLECTION;
    }
    if (dact_contains_ci(t, l, "that makes sense") || dact_contains_ci(t, l, "that's valid") ||
        dact_contains_ci(t, l, "thats valid")) {
        return HU_DACT_VALIDATION;
    }

    /* Question vs answer. */
    if (dact_ends_with_question(t, l)) {
        if (dact_contains_ci(t, l, "what do you mean") ||
            dact_contains_ci(t, l, "can you clarify") ||
            dact_contains_ci(t, l, "could you clarify")) {
            return HU_DACT_CLARIFY_QUESTION;
        }
        return HU_DACT_QUESTION;
    }

    return HU_DACT_ANSWER;
}
