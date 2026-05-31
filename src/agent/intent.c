/* Intent-aware response-type classifier. See include/human/agent/intent.h.
 *
 * Deterministic weighted scoring over message shape + word-boundary keyword
 * matches. NO LLM, no allocation in analyze (bounded stack scan buffer). Keyword
 * matching is hu_str_contains_word_ci (word boundary), never substring — so
 * "won" (GOOD_NEWS) does not fire inside "wonder", and "never" matches the word
 * but a vulnerable "never told" phrase still outscores a bare vent "never".
 */
#include "human/agent/intent.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_INTENT_SCAN_MAX 2048

static int kw(const char *buf, const char *needle) {
    return hu_str_contains_word_ci(buf, needle) ? 1 : 0;
}

void hu_intent_analyze(const char *msg, size_t len, hu_intent_analysis_t *out) {
    if (!out) {
        return;
    }
    out->intent = HU_INTENT_PROCESSING_ALOUD;
    out->confidence = 0.0;
    out->emotional_weight = 0.2;
    if (!msg || len == 0) {
        return;
    }

    char b[HU_INTENT_SCAN_MAX];
    size_t n = len < (HU_INTENT_SCAN_MAX - 1) ? len : (HU_INTENT_SCAN_MAX - 1);
    memcpy(b, msg, n);
    b[n] = '\0';

    /* shape features */
    size_t words = 0;
    int exclaim = 0;
    int question = 0;
    int inword = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c == '!') {
            exclaim++;
        } else if (c == '?') {
            question = 1;
        }
        if (isspace(c)) {
            inword = 0;
        } else if (!inword) {
            inword = 1;
            words++;
        }
    }
    int dbl_exclaim = exclaim >= 2;

    double s[HU_INTENT_COUNT];
    for (int i = 0; i < HU_INTENT_COUNT; i++) {
        s[i] = 0.0;
    }

    /* LOGISTICS — scheduling / concrete asks */
    if (kw(b, "what time"))
        s[HU_INTENT_LOGISTICS] += 0.5;
    if (kw(b, "you free"))
        s[HU_INTENT_LOGISTICS] += 0.4;
    if (kw(b, "around"))
        s[HU_INTENT_LOGISTICS] += 0.4;
    if (kw(b, "weekend"))
        s[HU_INTENT_LOGISTICS] += 0.4;
    if (kw(b, "tomorrow") || kw(b, "tonight") || kw(b, "today"))
        s[HU_INTENT_LOGISTICS] += 0.3;
    if (kw(b, "when") || kw(b, "where"))
        s[HU_INTENT_LOGISTICS] += 0.3;
    if (kw(b, "dinner") || kw(b, "lunch") || kw(b, "coffee"))
        s[HU_INTENT_LOGISTICS] += 0.3;
    if (kw(b, "meet") || kw(b, "grab"))
        s[HU_INTENT_LOGISTICS] += 0.3;
    if (kw(b, "saturday") || kw(b, "sunday") || kw(b, "monday") || kw(b, "tuesday") ||
        kw(b, "wednesday") || kw(b, "thursday") || kw(b, "friday"))
        s[HU_INTENT_LOGISTICS] += 0.3;
    if (question)
        s[HU_INTENT_LOGISTICS] += 0.15;

    /* SEEKING_ADVICE — explicit ask for a take */
    if (kw(b, "what do you think") || kw(b, "what would you do"))
        s[HU_INTENT_SEEKING_ADVICE] += 0.5;
    if (kw(b, "should i") || kw(b, "what should"))
        s[HU_INTENT_SEEKING_ADVICE] += 0.5;
    if (kw(b, "any advice") || kw(b, "advice"))
        s[HU_INTENT_SEEKING_ADVICE] += 0.4;
    if (kw(b, "how do i"))
        s[HU_INTENT_SEEKING_ADVICE] += 0.4;
    if (kw(b, "suggestion") || kw(b, "would you") || kw(b, "help me"))
        s[HU_INTENT_SEEKING_ADVICE] += 0.3;
    if (question)
        s[HU_INTENT_SEEKING_ADVICE] += 0.2;

    /* NEEDS_TO_BE_HEARD — long + emotional, no explicit ask */
    if (words > 50)
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.5;
    if (kw(b, "i feel") || kw(b, "feeling"))
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.3;
    if (kw(b, "i've been") || kw(b, "ive been"))
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.2;
    if (kw(b, "overwhelmed") || kw(b, "exhausted") || kw(b, "exhausting"))
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.3;
    if (kw(b, "anxious") || kw(b, "stressed") || kw(b, "lonely"))
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.3;
    if (kw(b, "scared") || kw(b, "hurt") || kw(b, "sad"))
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.2;
    if (kw(b, "barely sleep"))
        s[HU_INTENT_NEEDS_TO_BE_HEARD] += 0.3;

    /* JUST_VENTING — frustration, not seeking a fix */
    if (kw(b, "ugh") || kw(b, "argh"))
        s[HU_INTENT_JUST_VENTING] += 0.3;
    if (kw(b, "can't believe") || kw(b, "cant believe"))
        s[HU_INTENT_JUST_VENTING] += 0.4;
    if (kw(b, "frustrated") || kw(b, "frustrating") || kw(b, "pissed"))
        s[HU_INTENT_JUST_VENTING] += 0.4;
    if (kw(b, "drives me crazy"))
        s[HU_INTENT_JUST_VENTING] += 0.4;
    if (kw(b, "annoyed") || kw(b, "annoying"))
        s[HU_INTENT_JUST_VENTING] += 0.3;
    if (kw(b, "always") || kw(b, "never") || kw(b, "every time"))
        s[HU_INTENT_JUST_VENTING] += 0.3;
    if (kw(b, "seriously"))
        s[HU_INTENT_JUST_VENTING] += 0.2;
    if (dbl_exclaim)
        s[HU_INTENT_JUST_VENTING] += 0.2;

    /* VULNERABLE_SHARE — disclosure / trust */
    if (kw(b, "never told"))
        s[HU_INTENT_VULNERABLE_SHARE] += 0.6;
    if (kw(b, "scared to admit") || kw(b, "ashamed"))
        s[HU_INTENT_VULNERABLE_SHARE] += 0.5;
    if (kw(b, "the truth is") || kw(b, "hard to say") || kw(b, "between us") ||
        kw(b, "don't judge") || kw(b, "dont judge"))
        s[HU_INTENT_VULNERABLE_SHARE] += 0.4;
    if (kw(b, "embarrassed") || kw(b, "vulnerable"))
        s[HU_INTENT_VULNERABLE_SHARE] += 0.4;
    if (kw(b, "honestly") || kw(b, "to be honest"))
        s[HU_INTENT_VULNERABLE_SHARE] += 0.3;

    /* GOOD_NEWS — celebration */
    if (kw(b, "i did it") || kw(b, "got the job") || kw(b, "amazing news") || kw(b, "great news") ||
        kw(b, "engaged"))
        s[HU_INTENT_GOOD_NEWS] += 0.5;
    if (kw(b, "guess what") || kw(b, "finally") || kw(b, "so happy") || kw(b, "so excited") ||
        kw(b, "accepted") || kw(b, "won"))
        s[HU_INTENT_GOOD_NEWS] += 0.4;
    if (kw(b, "passed"))
        s[HU_INTENT_GOOD_NEWS] += 0.3;
    if (dbl_exclaim)
        s[HU_INTENT_GOOD_NEWS] += 0.2;

    /* SMALL_TALK — light social */
    if (kw(b, "what's up") || kw(b, "whats up") || kw(b, "how's it going") ||
        kw(b, "hows it going") || kw(b, "how are you"))
        s[HU_INTENT_SMALL_TALK] += 0.4;
    if (kw(b, "sup"))
        s[HU_INTENT_SMALL_TALK] += 0.3;
    if (kw(b, "hey") || kw(b, "yo") || kw(b, "lol") || kw(b, "haha"))
        s[HU_INTENT_SMALL_TALK] += 0.2;

    /* pick max (first in enum order on ties) */
    int best = HU_INTENT_PROCESSING_ALOUD;
    double best_score = 0.0;
    for (int i = 1; i < HU_INTENT_COUNT; i++) {
        if (s[i] > best_score) {
            best_score = s[i];
            best = i;
        }
    }

    static const double ew[HU_INTENT_COUNT] = {
        [HU_INTENT_PROCESSING_ALOUD] = 0.2, [HU_INTENT_LOGISTICS] = 0.15,
        [HU_INTENT_SEEKING_ADVICE] = 0.4,   [HU_INTENT_NEEDS_TO_BE_HEARD] = 0.8,
        [HU_INTENT_JUST_VENTING] = 0.6,     [HU_INTENT_VULNERABLE_SHARE] = 0.9,
        [HU_INTENT_GOOD_NEWS] = 0.5,        [HU_INTENT_SMALL_TALK] = 0.2,
    };

    out->intent = (hu_intent_t)best;
    out->confidence = best_score > 1.0 ? 1.0 : best_score;
    out->emotional_weight = ew[best];
}

static const char *const NAMES[HU_INTENT_COUNT] = {
    "processing_aloud", "logistics",        "seeking_advice", "needs_to_be_heard",
    "just_venting",     "vulnerable_share", "good_news",      "small_talk",
};

/* Terse reply-strategy directives. Keep each <= ~140 chars (prompt is already
 * ~16 KB and latency-sensitive). PROCESSING_ALOUD has none (injects nothing). */
static const char *const DIRECTIVES[HU_INTENT_COUNT] = {
    [HU_INTENT_PROCESSING_ALOUD] = NULL,
    [HU_INTENT_LOGISTICS] =
        "\n[intent: logistics] They want a quick, concrete answer. Reply short and direct — no "
        "stories or elaboration.",
    [HU_INTENT_SEEKING_ADVICE] =
        "\n[intent: seeking_advice] They're asking for your take. Give a brief, honest opinion; "
        "don't lecture.",
    [HU_INTENT_NEEDS_TO_BE_HEARD] =
        "\n[intent: needs_to_be_heard] They need to be heard, not fixed. Acknowledge and listen; "
        "keep it short.",
    [HU_INTENT_JUST_VENTING] =
        "\n[intent: just_venting] They're venting. Validate the frustration; don't problem-solve "
        "or redirect.",
    [HU_INTENT_VULNERABLE_SHARE] =
        "\n[intent: vulnerable_share] They shared something vulnerable. Hold space and acknowledge "
        "the trust; no advice.",
    [HU_INTENT_GOOD_NEWS] =
        "\n[intent: good_news] Good news — match their energy and celebrate with them.",
    [HU_INTENT_SMALL_TALK] =
        "\n[intent: small_talk] Light social chat. Keep it warm, casual, and brief.",
};

const char *hu_intent_name(hu_intent_t intent) {
    if ((int)intent < 0 || (int)intent >= HU_INTENT_COUNT) {
        return "unknown";
    }
    return NAMES[intent];
}

hu_error_t hu_intent_build_directive(const hu_allocator_t *alloc, const hu_intent_analysis_t *a,
                                     char **dir, size_t *dir_len) {
    if (!alloc || !a || !dir || !dir_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *dir = NULL;
    *dir_len = 0;
    if (a->intent == HU_INTENT_PROCESSING_ALOUD || a->confidence < HU_INTENT_CONFIDENCE_THRESHOLD) {
        return HU_OK; /* nothing to inject */
    }
    const char *text = DIRECTIVES[a->intent];
    if (!text) {
        return HU_OK;
    }
    size_t tlen = strlen(text);
    char *buf = (char *)alloc->realloc(alloc->ctx, NULL, 0, tlen + 1);
    if (!buf) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, text, tlen);
    buf[tlen] = '\0';
    *dir = buf;
    *dir_len = tlen;
    return HU_OK;
}
