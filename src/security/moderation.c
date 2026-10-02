#include "human/security/moderation.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/security/normalize.h"
#include "human/security/self_harm.h"
#include <ctype.h>
#include <string.h>

/* Case-insensitive substring / whole-word matching: the shared string helpers. */
static bool mod_contains(const char *text, size_t len, const char *word) {
    return hu_str_contains_ci_cstr(text, len, word);
}

/* True when "kill" at norm[i..] is the k-i-l-l inside "skill", not a standalone token. */
static bool mod_kill_match_is_skill_false_positive(const char *norm, size_t len, size_t i) {
    if (i == 0 || i + 3 >= len)
        return false;
    if (norm[i - 1] != 's')
        return false;
    if (strncmp(norm + i, "kill", 4) != 0)
        return false;
    if (i + 4 < len) {
        if (norm[i + 4] == 'l')
            return (i + 5 == len || !isalnum((unsigned char)norm[i + 5]));
        return false;
    }
    return true;
}

static bool mod_norm_has_kill_not_skill(const char *norm, size_t len) {
    if (len < 4)
        return false;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (strncmp(norm + i, "kill", 4) != 0)
            continue;
        if (mod_kill_match_is_skill_false_positive(norm, len, i))
            continue;
        return true;
    }
    return false;
}

static bool mod_norm_has_violence_not_nonprefix(const char *norm, size_t len) {
    static const char w[] = "violence";
    const size_t wlen = sizeof(w) - 1;
    if (len < wlen)
        return false;
    for (size_t i = 0; i + wlen <= len; i++) {
        if (strncmp(norm + i, w, wlen) != 0)
            continue;
        if (i >= 3 && strncmp(norm + i - 3, "non", 3) == 0)
            continue;
        return true;
    }
    return false;
}

static bool mod_contains_word(const char *text, size_t len, const char *word) {
    return hu_str_contains_word_ci_n(text, len, word);
}

static bool mod_violence_hit(const char *text, size_t text_len, const char *norm, size_t norm_len) {
    return mod_contains_word(text, text_len, "kill") || mod_contains_word(norm, norm_len, "kill") ||
           mod_norm_has_kill_not_skill(norm, norm_len) ||
           mod_contains_word(text, text_len, "murder") ||
           mod_contains_word(norm, norm_len, "murder") || mod_contains(norm, norm_len, "murder") ||
           mod_contains_word(text, text_len, "violence") ||
           mod_contains_word(norm, norm_len, "violence") ||
           mod_norm_has_violence_not_nonprefix(norm, norm_len);
}

/* The pre-2026-10-02 self-harm lists, kept byte-identical for
 * HU_CRISIS_TIERS=off. Graduated severity: high-specificity phrases get full
 * score; ambiguous frustration phrases a lower, unflagged score. */
static void mod_legacy_self_harm(const char *text, size_t text_len, const char *norm,
                                 size_t norm_len, hu_moderation_result_t *out) {
    int high_sev = 0, low_sev = 0;
    if (mod_contains_word(text, text_len, "suicide") || mod_contains(norm, norm_len, "suicide"))
        high_sev++;
    if (mod_contains(text, text_len, "self harm") || mod_contains(text, text_len, "self-harm") ||
        mod_contains(norm, norm_len, "selfharm") || mod_contains(norm, norm_len, "self-harm"))
        high_sev++;
    if (mod_contains_word(text, text_len, "kms") || mod_contains(norm, norm_len, "kms"))
        high_sev++;
    if (mod_contains_word(text, text_len, "unalive") || mod_contains(norm, norm_len, "unalive"))
        high_sev++;
    if (mod_contains(text, text_len, "end it all") || mod_contains(norm, norm_len, "enditall"))
        high_sev++;
    if (mod_contains(text, text_len, "better off without me") ||
        mod_contains(norm, norm_len, "betteroffwithoutme"))
        high_sev++;
    if (mod_contains(text, text_len, "don't want to be here anymore") ||
        mod_contains(norm, norm_len, "don'twanttobehereanymore") ||
        mod_contains(norm, norm_len, "dontwanttobehereanymore"))
        high_sev++;
    if (mod_contains(text, text_len, "no reason to go on") ||
        mod_contains(norm, norm_len, "noreasontogoon"))
        high_sev++;
    if (mod_contains(text, text_len, "what's the point") ||
        mod_contains(norm, norm_len, "what'sthepoint") ||
        mod_contains(norm, norm_len, "whatsthepoint"))
        low_sev++;
    if (mod_contains(text, text_len, "i can't do this anymore") ||
        mod_contains(norm, norm_len, "ican'tdothisanymore") ||
        mod_contains(norm, norm_len, "icantdothisanymore"))
        low_sev++;

    if (high_sev > 0) {
        out->self_harm = true;
        out->self_harm_score = 0.85;
        out->flagged = true;
        out->self_harm_tier = HU_SELF_HARM_EXPLICIT;
    } else if (low_sev > 0) {
        out->self_harm = true;
        out->self_harm_score = 0.35 + (double)low_sev * 0.1;
        /* flagged only at high score — low-severity signals empathy, not crisis */
        out->flagged = (out->self_harm_score >= 0.6);
        out->self_harm_tier = out->flagged ? HU_SELF_HARM_EXPLICIT : HU_SELF_HARM_LOW;
    }
}

/* HU_CRISIS_TIERS live: the one detector (self_harm.c). A self-harm phrase
 * that contains a kill-word is blanked before the violence check, so "kill
 * myself" is self-harm, not violence against others. */
static hu_error_t mod_tiered_self_harm_and_violence(const char *text, size_t text_len,
                                                    hu_moderation_result_t *out) {
    hu_self_harm_tier_t tier = hu_self_harm_classify(text, text_len);
    out->self_harm_tier = tier;
    if (tier == HU_SELF_HARM_EXPLICIT) {
        out->self_harm = true;
        out->self_harm_score = 0.85;
        out->flagged = true;
    } else if (tier == HU_SELF_HARM_LOW) {
        out->self_harm = true;
        out->self_harm_score = 0.45; /* empathy, not crisis: unflagged */
    }
    char masked[4096];
    size_t masked_len = hu_self_harm_mask_kill_phrases(text, text_len, masked, sizeof(masked));
    char mnorm[4096];
    size_t mnorm_len = 0;
    hu_error_t nerr =
        hu_normalize_confusables(masked, masked_len, mnorm, sizeof(mnorm), &mnorm_len);
    if (nerr != HU_OK)
        return nerr;
    /* past the mask buffer, the raw tail is still checked unmasked */
    bool tail_hit = text_len > masked_len &&
                    mod_contains_word(text + masked_len, text_len - masked_len, "kill");
    if (mod_violence_hit(masked, masked_len, mnorm, mnorm_len) || tail_hit) {
        out->violence = true;
        out->violence_score = 0.9;
        out->flagged = true;
    }
    return HU_OK;
}

hu_error_t hu_moderation_check_local(hu_allocator_t *alloc, const char *text, size_t text_len,
                                     hu_moderation_result_t *out) {
    if (!alloc || !text || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char norm[4096];
    size_t norm_len = 0;
    hu_error_t nerr = hu_normalize_confusables(text, text_len, norm, sizeof(norm), &norm_len);
    if (nerr != HU_OK)
        return nerr;

    if (norm_len >= sizeof(norm) - 1) {
        hu_log_warn("moderation", NULL,
                    "normalization truncated (%zu bytes input, %zu norm cap) — "
                    "tail content checked by raw-text only",
                    text_len, sizeof(norm));
    }

    bool tiered = hu_crisis_tiers_mode() == HU_GATE_LIVE;
    if (tiered) {
        nerr = mod_tiered_self_harm_and_violence(text, text_len, out);
        if (nerr != HU_OK)
            return nerr;
    } else if (mod_violence_hit(text, text_len, norm, norm_len)) {
        out->violence = true;
        out->violence_score = 0.9;
        out->flagged = true;
    }
    if ((mod_contains_word(text, text_len, "hate") && mod_contains_word(text, text_len, "group")) ||
        (mod_contains(norm, norm_len, "hate") && mod_contains(norm, norm_len, "group"))) {
        out->hate = true;
        out->hate_score = 0.8;
        out->flagged = true;
    }
    if (!tiered)
        mod_legacy_self_harm(text, text_len, norm, norm_len, out);
    if (mod_contains(text, text_len, "explicit sexual") ||
        mod_contains(norm, norm_len, "explicitsexual") ||
        mod_contains_word(text, text_len, "nsfw") || mod_contains(norm, norm_len, "nsfw") ||
        mod_contains(text, text_len, "pornograph")) {
        out->sexual = true;
        out->sexual_score = 0.7;
        out->flagged = true;
    }
    return HU_OK;
}

hu_error_t hu_moderation_check(hu_allocator_t *alloc, const char *text, size_t text_len,
                               hu_moderation_result_t *out) {
    return hu_moderation_check_local(alloc, text, text_len, out);
}

hu_error_t hu_crisis_response_build(hu_allocator_t *alloc, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    static const char msg[] = "If you're in crisis, please reach out: "
                              "988 Suicide & Crisis Lifeline (call/text 988), "
                              "Crisis Text Line (text HOME to 741741)";
    *out = (char *)alloc->alloc(alloc->ctx, sizeof(msg));
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, msg, sizeof(msg));
    *out_len = sizeof(msg) - 1;
    return HU_OK;
}
