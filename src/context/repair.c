/* Conversation repair: detect user repair attempts and metacognition-driven
 * degradation, then produce directives for the next response.
 * See arXiv:2505.06120 — LLMs compound errors rather than recovering. */
#include "human/context/repair.h"
#include "human/core/string.h"

#include <ctype.h>
#include <string.h>

/* ── case-insensitive substring search ────────────────────────────── */

/* ── pattern tables ───────────────────────────────────────────────── */

static const char *const CORRECTION_PATTERNS[] = {
    "no,",
    "no that's",
    "that's not right",
    "that's wrong",
    "that is wrong",
    "incorrect",
    "i didn't say that",
    "i never said that",
    "you're wrong",
    "you are wrong",
    "not what i said",
    "not what i meant",
    NULL,
};

static const char *const REDIRECT_PATTERNS[] = {
    "i was asking about",
    "i meant",
    "not that, i mean",
    "going off on a tangent",
    "off topic",
    "off-topic",
    "that's not what i asked",
    "back to my question",
    "let me rephrase",
    "what i actually asked",
    NULL,
};

static const char *const CONFUSION_PATTERNS[] = {
    "what?",
    "huh?",
    "that doesn't make sense",
    "that makes no sense",
    "i'm confused",
    "i am confused",
    "i don't understand",
    "i dont understand",
    "what are you talking about",
    "that's nonsensical",
    NULL,
};

static const char *const MEMORY_ERROR_PATTERNS[] = {
    "you're confusing me with",
    "you are confusing me with",
    "i never told you",
    "i never said",
    "that never happened",
    "wrong person",
    "that's someone else",
    "that isn't me",
    "you're mixing me up",
    "you are mixing me up",
    NULL,
};

/* ── directives ───────────────────────────────────────────────────── */

static const char DIRECTIVE_CORRECTION[] =
    "The user is correcting you. Acknowledge the specific correction. "
    "Do not argue or rationalize.";

static const char DIRECTIVE_REDIRECT[] =
    "You went off-topic. Acknowledge it briefly and return to what the "
    "user actually asked about.";

static const char DIRECTIVE_CONFUSION[] =
    "The user is confused by your response. Simplify your explanation "
    "and check your reasoning for errors.";

static const char DIRECTIVE_MEMORY_ERROR[] =
    "The user says you got a personal detail wrong. Apologize specifically, "
    "ask for the correct information, and commit to remembering it.";

static const char DIRECTIVE_SELF_DETECTED[] =
    "You may be going off track. Consider pausing and asking: "
    "'Am I understanding you correctly?'";

/* ── helpers ──────────────────────────────────────────────────────── */

static size_t count_matches(const char *msg, size_t msg_len,
                            const char *const *patterns) {
    size_t hits = 0;
    for (size_t i = 0; patterns[i]; i++) {
        if (hu_str_contains_ci_cstr(msg, msg_len, patterns[i]))
            hits++;
    }
    return hits;
}

static void fill_signal(hu_repair_signal_t *out, hu_repair_type_t type,
                         float confidence, const char *directive,
                         bool acknowledge) {
    out->type = type;
    out->confidence = confidence;
    out->should_acknowledge = acknowledge;
    size_t dlen = strlen(directive);
    if (dlen >= HU_REPAIR_DIRECTIVE_CAP)
        dlen = HU_REPAIR_DIRECTIVE_CAP - 1;
    memcpy(out->directive, directive, dlen);
    out->directive[dlen] = '\0';
}

/* Check if a short message starts with bare "no" (word boundary).
 * Avoids false positives on "not bad", "nobody", etc. */
static bool starts_with_bare_no(const char *msg, size_t msg_len) {
    if (msg_len < 2)
        return false;
    if (tolower((unsigned char)msg[0]) != 'n' || tolower((unsigned char)msg[1]) != 'o')
        return false;
    if (msg_len == 2)
        return true;
    char after = msg[2];
    return after == ' ' || after == ',' || after == '.' || after == '!' || after == '\0';
}

/* ── public API ───────────────────────────────────────────────────── */

hu_error_t hu_repair_detect(const char *user_message, size_t msg_len,
                            hu_repair_signal_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!user_message || msg_len == 0)
        return HU_OK;

    /* Score each category. */
    size_t correction_hits = count_matches(user_message, msg_len, CORRECTION_PATTERNS);
    size_t redirect_hits   = count_matches(user_message, msg_len, REDIRECT_PATTERNS);
    size_t confusion_hits  = count_matches(user_message, msg_len, CONFUSION_PATTERNS);
    size_t memory_hits     = count_matches(user_message, msg_len, MEMORY_ERROR_PATTERNS);

    /* Bare "no" at start of short messages is a correction signal. */
    if (correction_hits == 0 && msg_len < 40 && starts_with_bare_no(user_message, msg_len))
        correction_hits++;

    /* Pick highest-scoring category. Memory errors are most specific, so
     * they win ties over correction; redirect wins over confusion. */
    size_t best = 0;
    hu_repair_type_t best_type = HU_REPAIR_NONE;

    if (memory_hits > best) {
        best = memory_hits;
        best_type = HU_REPAIR_MEMORY_ERROR;
    }
    if (correction_hits > best) {
        best = correction_hits;
        best_type = HU_REPAIR_USER_CORRECTION;
    }
    if (redirect_hits > best) {
        best = redirect_hits;
        best_type = HU_REPAIR_USER_REDIRECT;
    }
    if (confusion_hits > best) {
        best = confusion_hits;
        best_type = HU_REPAIR_USER_CONFUSION;
    }

    if (best_type == HU_REPAIR_NONE)
        return HU_OK;

    /* Confidence: one match = 0.6, two = 0.8, three+ = 0.95. */
    float conf = 0.6f;
    if (best >= 3)
        conf = 0.95f;
    else if (best >= 2)
        conf = 0.8f;

    const char *directive = NULL;
    bool acknowledge = true;
    switch (best_type) {
    case HU_REPAIR_USER_CORRECTION:
        directive = DIRECTIVE_CORRECTION;
        break;
    case HU_REPAIR_USER_REDIRECT:
        directive = DIRECTIVE_REDIRECT;
        break;
    case HU_REPAIR_USER_CONFUSION:
        directive = DIRECTIVE_CONFUSION;
        acknowledge = false; /* don't add "I got confused" for user confusion */
        break;
    case HU_REPAIR_MEMORY_ERROR:
        directive = DIRECTIVE_MEMORY_ERROR;
        break;
    default:
        return HU_OK; /* unreachable */
    }

    fill_signal(out, best_type, conf, directive, acknowledge);
    return HU_OK;
}

hu_error_t hu_repair_from_metacognition(bool is_degrading, float coherence,
                                        unsigned consecutive_degrading,
                                        hu_repair_signal_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!is_degrading)
        return HU_OK;

    /* Require 2+ consecutive degrading signals before emitting a repair. */
    if (consecutive_degrading < 2)
        return HU_OK;

    float conf = 0.5f + (1.0f - coherence) * 0.3f;
    if (conf > 1.0f)
        conf = 1.0f;
    if (consecutive_degrading >= 3)
        conf += 0.15f;
    if (conf > 1.0f)
        conf = 1.0f;

    fill_signal(out, HU_REPAIR_SELF_DETECTED, conf, DIRECTIVE_SELF_DETECTED, true);
    return HU_OK;
}

const char *hu_repair_type_name(hu_repair_type_t type) {
    switch (type) {
    case HU_REPAIR_NONE:            return "none";
    case HU_REPAIR_USER_CORRECTION: return "user_correction";
    case HU_REPAIR_USER_REDIRECT:   return "user_redirect";
    case HU_REPAIR_USER_CONFUSION:  return "user_confusion";
    case HU_REPAIR_MEMORY_ERROR:    return "memory_error";
    case HU_REPAIR_SELF_DETECTED:   return "self_detected";
    default:                        return "unknown";
    }
}
