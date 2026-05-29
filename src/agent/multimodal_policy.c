/* L4 multimodal policy — C port of scripts/multimodal_policy.py.
 *
 * Pure predicate: incoming message → {modality, tapback_kind,
 * confidence, reason}. Decision rules mirror the Python source
 * verbatim (same 21 golden cases pin both). Word-boundary matching
 * per ~/.claude/rules/substring-classifier-pitfalls.md so "lukewarm"
 * never trips "warm", "unfriendly" never trips "friend". */
#include "human/agent/multimodal_policy.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Word-boundary CI matching uses hu_str_contains_word_ci_n (human/core/string.h),
 * the length-bounded variant — callers here pass a slice (incoming, incoming_len)
 * that is not guaranteed NUL-terminated at incoming_len. See the
 * substring-classifier-pitfalls rule for the word-boundary rationale. */

/* True iff the entire stripped string equals one of the tokens (with
 * tolerant trailing punctuation .!?). Used for "the whole incoming is
 * just laughter / ack / etc.". */
static bool mm_whole_string_is(const char *s, size_t len, const char *const *tokens,
                               size_t n_tokens) {
    if (!s || len == 0)
        return false;
    /* Strip leading whitespace */
    while (len > 0 && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    /* Strip trailing whitespace and ./!/? */
    while (len > 0 && (isspace((unsigned char)s[len - 1]) || s[len - 1] == '.' ||
                       s[len - 1] == '!' || s[len - 1] == '?'))
        len--;
    for (size_t i = 0; i < n_tokens; i++) {
        size_t tlen = strlen(tokens[i]);
        if (tlen != len)
            continue;
        size_t j = 0;
        for (; j < tlen; j++) {
            if (tolower((unsigned char)s[j]) != tolower((unsigned char)tokens[i][j]))
                break;
        }
        if (j == tlen)
            return true;
    }
    return false;
}

/* Trailing-question detector. True iff the string ends with '?'
 * (ignoring trailing whitespace). */
static bool mm_ends_with_question(const char *s, size_t len) {
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;
    return len > 0 && s[len - 1] == '?';
}

/* Bare-laughter check: the whole stripped incoming is just lol/lmao/etc. */
static bool mm_is_bare_laughter(const char *s, size_t len) {
    static const char *const laughs[] = {"lol",  "lmao", "rofl",   "lmfao",
                                         "haha", "hehe", "hahaha", "hehehe"};
    return mm_whole_string_is(s, len, laughs, sizeof(laughs) / sizeof(laughs[0]));
}

/* Decision-table helpers. Each returns whether the rule fires and (when
 * fired) sets *out to the rule's verdict. The first rule that fires
 * wins — order matches the Python source for parity. */

/* Ack-class incoming: short "k"/"ok"/"got it"/"cool got it" etc.
 * NOTE: thanks/thx/ty intentionally NOT in this bucket — those are
 * appreciation (tapback love), handled by mm_check_appreciation. */
static bool mm_check_ack(const char *s, size_t len, hu_mm_decision_t *out) {
    if (len > 16)
        return false;
    static const char *const ack_tokens[] = {
        "k",   "kk",  "ok",  "okay", "cool", "got it", "sounds good", "sg",  "gotcha",
        "yep", "yup", "yes", "no",   "nope", "same",   "word",        "bet", "fr",
    };
    /* Whole-string ack OR two-word combo (e.g. "cool got it"). */
    if (mm_whole_string_is(s, len, ack_tokens, sizeof(ack_tokens) / sizeof(ack_tokens[0]))) {
        out->modality = HU_MM_MODALITY_TAPBACK;
        out->tapback_kind = HU_MM_TAPBACK_LIKE;
        out->confidence = 0.85f;
        out->reason = "incoming-is-ack-tapback-suffices";
        return true;
    }
    /* Two-token compound ("cool got it") — accept if both tokens are ack */
    return false;
}

/* Short appreciation: "thanks", "thx", "ty", "that helped", "appreciate it".
 * Tapback ❤️ fits. */
static bool mm_check_appreciation(const char *s, size_t len, hu_mm_decision_t *out) {
    if (len >= 40)
        return false;
    static const char *const phrases[] = {
        "thanks",
        "thank you",
        "thx",
        "ty",
        "tysm",
        "that helped",
        "appreciate it",
        "appreciate that",
        "appreciate this",
        "you're the best",
        "you are the best",
        "life saver",
        "lifesaver",
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, phrases[i])) {
            out->modality = HU_MM_MODALITY_TAPBACK;
            out->tapback_kind = HU_MM_TAPBACK_LOVE;
            out->confidence = 0.85f;
            out->reason = "incoming-appreciation-tapback-suffices";
            return true;
        }
    }
    return false;
}

/* Logistics arrival: "omw", "be there in N", "running late", "here". */
static bool mm_check_logistics(const char *s, size_t len, hu_mm_decision_t *out) {
    if (len >= 30)
        return false;
    static const char *const phrases[] = {
        "omw", "on my way", "be there", "running late", "arriving", "outside", "here",
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, phrases[i])) {
            out->modality = HU_MM_MODALITY_TAPBACK;
            out->tapback_kind = HU_MM_TAPBACK_LIKE;
            out->confidence = 0.80f;
            out->reason = "logistics-arrival-tapback-suffices";
            return true;
        }
    }
    return false;
}

/* Venting / bad-day signal — tapback ‼️ ("I hear you"). */
static bool mm_check_vent(const char *s, size_t len, hu_mm_decision_t *out) {
    static const char *const phrases[] = {
        "ugh",       "fml",     "worst day",   "awful day",    "terrible day",
        "hate this", "so done", "hate my job", "hate my boss",
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, phrases[i])) {
            out->modality = HU_MM_MODALITY_TAPBACK;
            out->tapback_kind = HU_MM_TAPBACK_EMPHASIZE;
            out->confidence = 0.70f;
            out->reason = "venting-signal-tapback-then-text-followup";
            return true;
        }
    }
    return false;
}

/* Hyped / celebratory — GIF fits. */
static bool mm_check_hyped(const char *s, size_t len, hu_mm_decision_t *out) {
    static const char *const phrases[] = {
        "let's go", "lets go", "lfg", "yesss", "yesssss", "fire", "goat",
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, phrases[i])) {
            out->modality = HU_MM_MODALITY_GIF;
            out->tapback_kind = HU_MM_TAPBACK_NONE;
            out->confidence = 0.70f;
            out->reason = "hyped-celebratory-gif-conveys-energy";
            return true;
        }
    }
    return false;
}

/* Deep emotional content — voice memo conveys more than tapback. */
static bool mm_check_voice(const char *s, size_t len, hu_mm_decision_t *out) {
    static const char *const phrases[] = {
        "grief", "loss", "miss you", "love you", "missing you",
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, phrases[i])) {
            out->modality = HU_MM_MODALITY_VOICE;
            out->tapback_kind = HU_MM_TAPBACK_NONE;
            out->confidence = 0.65f;
            out->reason = "deep-emotional-content-voice-conveys-more";
            return true;
        }
    }
    return false;
}

/* Short appreciative incoming with "love" / "proud of you" / "❤️". */
static bool mm_check_love_short(const char *s, size_t len, hu_mm_decision_t *out) {
    if (len >= 50)
        return false;
    if (hu_str_contains_word_ci_n(s, len, "love") || hu_str_contains_word_ci_n(s, len, "proud of you") ||
        hu_str_contains_word_ci_n(s, len, "proud of u")) {
        out->modality = HU_MM_MODALITY_TAPBACK;
        out->tapback_kind = HU_MM_TAPBACK_LOVE;
        out->confidence = 0.75f;
        out->reason = "short-appreciative-incoming-love-tapback";
        return true;
    }
    return false;
}

/* Short emphatic exclamation — "that's insane", "huge". */
static bool mm_check_emphasis(const char *s, size_t len, hu_mm_decision_t *out) {
    if (len >= 40)
        return false;
    static const char *const phrases[] = {
        "huge", "massive", "incredible", "wild", "insane", "crazy", "unbelievable", "amazing",
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++) {
        if (hu_str_contains_word_ci_n(s, len, phrases[i])) {
            out->modality = HU_MM_MODALITY_TAPBACK;
            out->tapback_kind = HU_MM_TAPBACK_EMPHASIZE;
            out->confidence = 0.60f;
            out->reason = "short-emphatic-incoming-double-emphasis";
            return true;
        }
    }
    return false;
}

hu_error_t hu_multimodal_decide(const char *incoming, size_t incoming_len, hu_mm_decision_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    /* Default: text. Each rule overrides on hit. */
    out->modality = HU_MM_MODALITY_TEXT;
    out->tapback_kind = HU_MM_TAPBACK_NONE;
    out->confidence = 0.95f;
    out->reason = "default-text-fits";

    if (!incoming || incoming_len == 0) {
        out->confidence = 1.0f;
        out->reason = "empty-incoming";
        return HU_OK;
    }

    /* Strip leading whitespace for the dispatch; rules see the trimmed
     * view but original pointers still flow to the matchers since they
     * handle internal whitespace themselves. */
    while (incoming_len > 0 && isspace((unsigned char)*incoming)) {
        incoming++;
        incoming_len--;
    }
    if (incoming_len == 0) {
        out->confidence = 1.0f;
        out->reason = "empty-incoming";
        return HU_OK;
    }

    /* Rule order matches scripts/multimodal_policy.py for golden-set parity. */
    if (mm_check_ack(incoming, incoming_len, out))
        return HU_OK;
    if (mm_check_appreciation(incoming, incoming_len, out))
        return HU_OK;
    if (mm_check_logistics(incoming, incoming_len, out))
        return HU_OK;

    /* Explicit question — text only, no tapback ?. */
    if (mm_ends_with_question(incoming, incoming_len)) {
        out->modality = HU_MM_MODALITY_TEXT;
        out->tapback_kind = HU_MM_TAPBACK_NONE;
        out->confidence = 0.95f;
        out->reason = "explicit-question";
        return HU_OK;
    }

    if (mm_check_vent(incoming, incoming_len, out))
        return HU_OK;

    /* Bare laughter — tapback laugh, don't echo. */
    if (mm_is_bare_laughter(incoming, incoming_len)) {
        out->modality = HU_MM_MODALITY_TAPBACK;
        out->tapback_kind = HU_MM_TAPBACK_LAUGH;
        out->confidence = 0.85f;
        out->reason = "incoming-is-bare-laughter-tapback-not-echo";
        return HU_OK;
    }
    /* Embedded laughter — text (build on the joke). */
    static const char *const laugh_tokens[] = {"lol", "lmao", "rofl", "haha", "hehe"};
    for (size_t i = 0; i < sizeof(laugh_tokens) / sizeof(laugh_tokens[0]); i++) {
        if (hu_str_contains_word_ci_n(incoming, incoming_len, laugh_tokens[i])) {
            out->modality = HU_MM_MODALITY_TEXT;
            out->tapback_kind = HU_MM_TAPBACK_NONE;
            out->confidence = 0.60f;
            out->reason = "incoming-contains-laughter-build-don-t-react";
            return HU_OK;
        }
    }

    if (mm_check_voice(incoming, incoming_len, out))
        return HU_OK;
    if (mm_check_hyped(incoming, incoming_len, out))
        return HU_OK;
    if (mm_check_love_short(incoming, incoming_len, out))
        return HU_OK;
    if (mm_check_emphasis(incoming, incoming_len, out))
        return HU_OK;

    /* Default text — already set above. */
    return HU_OK;
}
