/* src/agent/salience.c — salience/arbitration layer (coherence, Gap 1).
 * See include/human/agent/salience.h for the contract. Reuses the arbitrator
 * engine; adds canonicalization (P3), Seth profile (P2), shadow rank (P4). */

#include "human/agent/salience.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Case-insensitive word-boundary substring match. A match counts only when it is
 * bounded by start/end of string or a non-alphanumeric char — so "lukewarm" does
 * NOT match "warm" and "unfriendly" does NOT match "friend"
 * (per .claude/rules/substring-classifier-pitfalls.md). */
static bool str_contains_word_ci(const char *s, size_t slen, const char *needle) {
    if (!s || !needle || !*needle)
        return false;
    size_t nlen = strlen(needle);
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

/* ---- P3: canonicalization ------------------------------------------------ */

uint32_t hu_salience_classify_source(const char *source, size_t source_len) {
    if (!source || source_len == 0)
        return HU_DIRECTIVE_BEHAVIORAL;
    uint32_t cat = 0;
    /* Each table entry contributes its bit when ANY of its keywords match. A
     * source may carry several categories (bitfield). */
    if (str_contains_word_ci(source, source_len, "safety") ||
        str_contains_word_ci(source, source_len, "boundary") ||
        str_contains_word_ci(source, source_len, "crisis") ||
        str_contains_word_ci(source, source_len, "deescalation"))
        cat |= HU_DIRECTIVE_SAFETY;
    if (str_contains_word_ci(source, source_len, "emotional") ||
        str_contains_word_ci(source, source_len, "emotion") ||
        str_contains_word_ci(source, source_len, "mood") ||
        str_contains_word_ci(source, source_len, "affect") ||
        str_contains_word_ci(source, source_len, "residue") ||
        str_contains_word_ci(source, source_len, "warmth") ||
        str_contains_word_ci(source, source_len, "comfort") ||
        str_contains_word_ci(source, source_len, "grief"))
        cat |= HU_DIRECTIVE_EMOTIONAL;
    if (str_contains_word_ci(source, source_len, "memory") ||
        str_contains_word_ci(source, source_len, "shared_reference") ||
        str_contains_word_ci(source, source_len, "reference") ||
        str_contains_word_ci(source, source_len, "recall") ||
        str_contains_word_ci(source, source_len, "episode") ||
        str_contains_word_ci(source, source_len, "opinion"))
        cat |= HU_DIRECTIVE_MEMORY;
    if (str_contains_word_ci(source, source_len, "curiosity") ||
        str_contains_word_ci(source, source_len, "proactive") ||
        str_contains_word_ci(source, source_len, "absence") ||
        str_contains_word_ci(source, source_len, "checkin") ||
        str_contains_word_ci(source, source_len, "followup") ||
        str_contains_word_ci(source, source_len, "anticipatory"))
        cat |= HU_DIRECTIVE_PROACTIVE;
    if (str_contains_word_ci(source, source_len, "somatic") ||
        str_contains_word_ci(source, source_len, "presence") ||
        str_contains_word_ci(source, source_len, "narrative") ||
        str_contains_word_ci(source, source_len, "creative") ||
        str_contains_word_ci(source, source_len, "growth") ||
        str_contains_word_ci(source, source_len, "identity") ||
        str_contains_word_ci(source, source_len, "persona") ||
        str_contains_word_ci(source, source_len, "voice"))
        cat |= HU_DIRECTIVE_IDENTITY;
    if (cat == 0)
        cat = HU_DIRECTIVE_BEHAVIORAL;
    return cat;
}

bool hu_salience_source_is_required(const char *source, size_t source_len) {
    if (!source || source_len == 0)
        return false;
    /* Risk R1/R2: never suppress a directive that handles safety, emotional
     * crisis, or a direct question. */
    return str_contains_word_ci(source, source_len, "safety") ||
           str_contains_word_ci(source, source_len, "crisis") ||
           str_contains_word_ci(source, source_len, "grief") ||
           str_contains_word_ci(source, source_len, "conflict") ||
           str_contains_word_ci(source, source_len, "boundary") ||
           str_contains_word_ci(source, source_len, "deescalation") ||
           str_contains_word_ci(source, source_len, "question");
}

hu_error_t hu_salience_build_candidate(hu_allocator_t *alloc, const char *source, size_t source_len,
                                       const char *content, size_t content_len,
                                       hu_directive_t *out) {
    if (!alloc || !out || !source || !content)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->source = hu_strndup(alloc, source, source_len);
    out->content = hu_strndup(alloc, content, content_len);
    if (!out->source || !out->content) {
        hu_directive_deinit(alloc, out);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->source_len = source_len;
    out->content_len = content_len;
    out->category = hu_salience_classify_source(source, source_len);
    out->required = hu_salience_source_is_required(source, source_len);
    out->token_cost = hu_directive_estimate_tokens(content, content_len);
    out->priority = 0.5; /* neutral base; caller may recompute via context factors */
    return HU_OK;
}

/* ---- P2: Seth profile ---------------------------------------------------- */

static void profile_add(hu_salience_profile_t *p, const char *kw, double w) {
    if (p->weight_count >= HU_SALIENCE_PROFILE_MAX_WEIGHTS)
        return;
    hu_salience_source_weight_t *e = &p->weights[p->weight_count++];
    snprintf(e->keyword, sizeof(e->keyword), "%s", kw);
    e->weight = w;
}

void hu_salience_profile_init_default(hu_salience_profile_t *p) {
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", "seth-default");
    p->default_weight = 1.0;
    /* Shaped defaults: a concrete shared memory or an emotional read is what a real
     * person foregrounds; generic always-on persona texture (somatic, narrative,
     * presence) and reflexive curiosity are toned down so they rarely win a turn. */
    profile_add(p, "shared_reference", 1.6);
    profile_add(p, "reference", 1.4);
    profile_add(p, "emotional", 1.4);
    profile_add(p, "residue", 1.2);
    profile_add(p, "memory", 1.3);
    profile_add(p, "humor", 1.1);
    profile_add(p, "curiosity", 0.5);
    profile_add(p, "absence", 0.6);
    profile_add(p, "somatic", 0.4);
    profile_add(p, "narrative", 0.4);
    profile_add(p, "presence", 0.5);
    profile_add(p, "creative", 0.5);
    profile_add(p, "growth", 0.5);
}

double hu_salience_profile_weight(const hu_salience_profile_t *p, const char *source,
                                  size_t source_len) {
    if (!p || !source || source_len == 0)
        return 1.0;
    for (size_t i = 0; i < p->weight_count; i++) {
        if (str_contains_word_ci(source, source_len, p->weights[i].keyword))
            return p->weights[i].weight;
    }
    return p->default_weight > 0.0 ? p->default_weight : 1.0;
}

/* ---- P4: shadow rank ----------------------------------------------------- */

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

hu_error_t hu_salience_rank(hu_allocator_t *alloc, hu_directive_t *candidates,
                            size_t candidate_count, const hu_salience_profile_t *profile,
                            const hu_arbitration_config_t *config,
                            hu_arbitration_result_t *result) {
    if (!alloc || !result)
        return HU_ERR_INVALID_ARGUMENT;
    if (candidate_count > 0 && !candidates)
        return HU_ERR_INVALID_ARGUMENT;
    /* Modulate each candidate's priority by its Seth-profile weight. Required
     * directives keep their priority too but bypass selection in the arbitrator. */
    for (size_t i = 0; i < candidate_count; i++) {
        double w =
            hu_salience_profile_weight(profile, candidates[i].source, candidates[i].source_len);
        candidates[i].priority = clamp01(candidates[i].priority * w);
    }
    return hu_arbitrator_select(alloc, candidates, candidate_count, config, result);
}

static bool result_contains_source(const hu_arbitration_result_t *r, const char *src,
                                   size_t src_len) {
    for (size_t i = 0; i < r->selected_count; i++) {
        if (r->selected[i].source_len == src_len && r->selected[i].source && src &&
            strncmp(r->selected[i].source, src, src_len) == 0)
            return true;
    }
    return false;
}

char *hu_salience_summarize(hu_allocator_t *alloc, const hu_directive_t *candidates,
                            size_t candidate_count, const hu_arbitration_result_t *result) {
    if (!alloc || !result)
        return NULL;
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "salience(shadow): kept %zu/%zu [", result->selected_count,
                       candidate_count);
    for (size_t i = 0; i < result->selected_count && pos > 0 && (size_t)pos < sizeof(buf); i++) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s%.*s", i ? "," : "",
                        (int)result->selected[i].source_len,
                        result->selected[i].source ? result->selected[i].source : "");
    }
    if (pos > 0 && (size_t)pos < sizeof(buf))
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "] suppressed [");
    bool first = true;
    for (size_t i = 0; i < candidate_count && pos > 0 && (size_t)pos < sizeof(buf); i++) {
        if (result_contains_source(result, candidates[i].source, candidates[i].source_len))
            continue;
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s%.*s", first ? "" : ",",
                        (int)candidates[i].source_len,
                        candidates[i].source ? candidates[i].source : "");
        first = false;
    }
    if (pos > 0 && (size_t)pos < sizeof(buf))
        snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]");
    char *out = hu_strdup(alloc, buf);
    return out ? out : hu_strdup(alloc, "salience(shadow): summary unavailable");
}
