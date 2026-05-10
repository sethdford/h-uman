#include "human/memory/belief.h"

#include "human/core/allocator.h"
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
#include "human/provider.h"
#endif

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Clamp helper */
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Safe string copy that always NUL-terminates. */
static void safe_strncpy(char *dst, const char *src, size_t n) {
    if (!src || n == 0)
        return;
    size_t i;
    for (i = 0; i + 1 < n && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

hu_belief_t hu_belief_init(float mean, const char *source, int64_t now) {
    hu_belief_t b;
    memset(&b, 0, sizeof(b));
    b.mean = clampf(mean, 0.0f, 1.0f);
    /* Beta(1,1) with one observation: variance = mean*(1-mean). */
    b.variance = b.mean * (1.0f - b.mean);
    b.last_updated = now;
    b.prov_count = 1;
    safe_strncpy(b.prov[0].source, source ? source : "", sizeof(b.prov[0].source));
    b.prov[0].observed_at = now;
    b.prov[0].weight = 1.0f;
    return b;
}

hu_belief_t hu_belief_update(const hu_belief_t *prior, float observation,
                              const char *source, int64_t now) {
    if (!prior) {
        return hu_belief_init(observation, source, now);
    }

    observation = clampf(observation, 0.0f, 1.0f);

    hu_belief_t b = *prior;
    b.last_updated = now;

    float diff = observation - prior->mean;
    float abs_diff = diff < 0.0f ? -diff : diff;

    /* Effective observation count approximated from variance:
     * For Beta(a,b): variance = ab / ((a+b)^2*(a+b+1)).
     * We track n_eff = 1/variance as a precision proxy.
     * On each update we increment n_eff by 1. */
    float precision = (prior->variance > 1e-9f) ? (1.0f / prior->variance) : 1e6f;
    float n_eff = precision; /* current effective count */

    /* New mean: running average with weight proportional to n_eff. */
    float new_mean = (prior->mean * n_eff + observation) / (n_eff + 1.0f);
    new_mean = clampf(new_mean, 0.0f, 1.0f);

    /* Variance update:
     * Corroborating (|diff| < 0.5): variance shrinks — new obs agrees, precision grows.
     * Contradicting (|diff| >= 0.5): variance grows — uncertainty increases. */
    float new_variance;
    if (abs_diff < 0.5f) {
        /* Shrink: new precision = n_eff + 1; new variance = 1/new_precision. */
        float new_precision = n_eff + 1.0f;
        new_variance = 1.0f / new_precision;
    } else {
        /* Grow: contradicting obs add noise proportional to disagreement. */
        new_variance = prior->variance + abs_diff * abs_diff * (1.0f / (n_eff + 1.0f));
    }
    /* Clamp: variance lives in [0, 0.25] (max for a [0,1] variable). */
    b.variance = clampf(new_variance, 0.0f, 0.25f);
    b.mean = new_mean;

    /* Append provenance (ring buffer, wrapping at 4). */
    uint8_t slot = b.prov_count < 4 ? b.prov_count : (uint8_t)3;
    if (b.prov_count >= 4) {
        /* Shift left to make room at slot 3. */
        for (int i = 0; i < 3; i++)
            b.prov[i] = b.prov[i + 1];
    }
    safe_strncpy(b.prov[slot].source, source ? source : "", sizeof(b.prov[slot].source));
    b.prov[slot].observed_at = now;
    b.prov[slot].weight = 1.0f / (n_eff + 1.0f);
    if (b.prov_count < 4)
        b.prov_count++;

    return b;
}

hu_belief_t hu_belief_combine(const hu_belief_t *a, const hu_belief_t *b) {
    if (!a && !b) {
        hu_belief_t z;
        memset(&z, 0, sizeof(z));
        return z;
    }
    if (!a) return *b;
    if (!b) return *a;

    hu_belief_t out;
    memset(&out, 0, sizeof(out));

    /* Inverse-variance pooling (precision weighting). */
    if (a->variance < 1e-9f && b->variance < 1e-9f) {
        /* Both degenerate: simple average. */
        out.mean = (a->mean + b->mean) * 0.5f;
        out.variance = 0.0f;
    } else if (a->variance < 1e-9f) {
        out.mean = a->mean;
        out.variance = 0.0f;
    } else if (b->variance < 1e-9f) {
        out.mean = b->mean;
        out.variance = 0.0f;
    } else {
        float pa = 1.0f / a->variance;
        float pb = 1.0f / b->variance;
        float pt = pa + pb;
        out.mean = clampf((pa * a->mean + pb * b->mean) / pt, 0.0f, 1.0f);
        out.variance = clampf(1.0f / pt, 0.0f, 0.25f);
    }

    /* Merge provenance: take up to 4 across both. */
    uint8_t n = 0;
    for (uint8_t i = 0; i < a->prov_count && n < 4; i++, n++)
        out.prov[n] = a->prov[i];
    for (uint8_t i = 0; i < b->prov_count && n < 4; i++, n++)
        out.prov[n] = b->prov[i];
    out.prov_count = n;

    out.last_updated = a->last_updated > b->last_updated ? a->last_updated : b->last_updated;
    return out;
}

bool hu_belief_significantly_disagrees(const hu_belief_t *a, const hu_belief_t *b,
                                        float sigma_threshold) {
    if (!a || !b)
        return false;
    float diff = a->mean - b->mean;
    if (diff < 0.0f) diff = -diff;
    /* Combined spread: sqrt(var_a + var_b). */
    float spread = sqrtf(a->variance + b->variance);
    if (spread < 1e-9f)
        return diff > 1e-6f;
    return diff > sigma_threshold * spread;
}

/* ── Semantic conflict detector (deterministic fallback) ────────────── */

#define BELIEF_MAX_WORDS 128

static size_t tokenize_lower(const char *text, size_t len,
                             char words[][64], size_t max_words) {
    size_t count = 0;
    size_t i = 0;
    while (i < len && count < max_words) {
        while (i < len && !isalpha((unsigned char)text[i]))
            i++;
        if (i >= len)
            break;
        size_t start = i;
        while (i < len && isalpha((unsigned char)text[i]))
            i++;
        size_t wlen = i - start;
        if (wlen == 0 || wlen >= 64)
            continue;
        for (size_t j = 0; j < wlen; j++)
            words[count][j] = (char)tolower((unsigned char)text[start + j]);
        words[count][wlen] = '\0';
        count++;
    }
    return count;
}

static bool is_negation(const char *word) {
    return strcmp(word, "not") == 0 ||
           strcmp(word, "never") == 0 ||
           strcmp(word, "no") == 0 ||
           strcmp(word, "none") == 0 ||
           strcmp(word, "neither") == 0 ||
           strcmp(word, "nor") == 0 ||
           strcmp(word, "cannot") == 0 ||
           strcmp(word, "cant") == 0;
}

static bool starts_with_dont(const char *word) {
    if (strlen(word) < 4)
        return false;
    return (strncmp(word, "don", 3) == 0) ||
           (strncmp(word, "doesn", 5) == 0) ||
           (strncmp(word, "didn", 4) == 0) ||
           (strncmp(word, "won", 3) == 0 && word[3] == 't') ||
           (strncmp(word, "wasn", 4) == 0) ||
           (strncmp(word, "isn", 3) == 0 && word[3] == 't') ||
           (strncmp(word, "aren", 4) == 0) ||
           (strncmp(word, "hasn", 4) == 0) ||
           (strncmp(word, "haven", 5) == 0) ||
           (strncmp(word, "couldn", 6) == 0) ||
           (strncmp(word, "wouldn", 6) == 0) ||
           (strncmp(word, "shouldn", 7) == 0);
}

/* P2G — Initial variance prior keyed by provenance string.
 *
 * The variance is what `hu_graph_upsert_relation_ex` writes alongside
 * the (mean = scalar confidence) when a new relation row is created.
 * Higher variance means "we don't trust this observation as much yet";
 * the W14 reverify runner already grows variance over time, so this
 * function only sets the starting point.
 *
 * Sources are matched as case-insensitive prefix tokens up to the
 * first non-alphanumeric byte, so "imessage" matches both "imessage"
 * and "imessage:turn-42". This keeps the heuristic robust to
 * provenance strings that prepend channel info or turn IDs. */
static int prov_starts_with_token(const char *prov, size_t prov_len,
                                  const char *token) {
    size_t tl = strlen(token);
    if (prov_len < tl) return 0;
    /* Boundary check: require the next byte (if any) to be a non-letter
     * so "imessage_x" matches "imessage" but "imessagery" does not. */
    if (prov_len > tl) {
        char nxt = prov[tl];
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z') ||
            (nxt >= '0' && nxt <= '9'))
            return 0;
    }
    for (size_t i = 0; i < tl; i++) {
        char a = prov[i], b = token[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

float hu_belief_initial_variance_for_provenance(const char *prov, size_t prov_len) {
    if (!prov || prov_len == 0)
        return 0.05f;

    /* Direct messaging channels and explicit user statements: low
     * variance. We trust what the user typed — the source itself is
     * not the unknown. */
    static const char *const kLow[] = {
        "imessage", "telegram", "discord", "slack", "whatsapp",
        "signal", "sms", "facebook", "instagram", "messenger",
        "user-explicit", "user-statement", "agent_dialogue",
        "agent-dialogue", NULL,
    };
    for (size_t i = 0; kLow[i]; i++) {
        if (prov_starts_with_token(prov, prov_len, kLow[i]))
            return 0.02f;
    }

    /* Heuristic-derived: higher variance because the inference itself
     * is the unknown. Each of these labels marks a relation that was
     * not directly stated but synthesized by the system. */
    static const char *const kHigh[] = {
        "autodream", "consolidated", "inferred", "extracted",
        "extraction", "heuristic", "fallback", NULL,
    };
    for (size_t i = 0; kHigh[i]; i++) {
        if (prov_starts_with_token(prov, prov_len, kHigh[i]))
            return 0.10f;
    }

    return 0.05f;
}

hu_belief_conflict_t hu_belief_semantic_conflict(
    const char *text_a, size_t len_a,
    const char *text_b, size_t len_b) {
    if (!text_a || !text_b || len_a == 0 || len_b == 0)
        return HU_BELIEF_CONFLICT_NONE;

    char words_a[BELIEF_MAX_WORDS][64];
    char words_b[BELIEF_MAX_WORDS][64];
    size_t na = tokenize_lower(text_a, len_a, words_a, BELIEF_MAX_WORDS);
    size_t nb = tokenize_lower(text_b, len_b, words_b, BELIEF_MAX_WORDS);

    if (na == 0 || nb == 0)
        return HU_BELIEF_CONFLICT_NONE;

    /* Count negation words in each text. */
    size_t neg_a = 0, neg_b = 0;
    for (size_t i = 0; i < na; i++) {
        if (is_negation(words_a[i]) || starts_with_dont(words_a[i]))
            neg_a++;
    }
    for (size_t i = 0; i < nb; i++) {
        if (is_negation(words_b[i]) || starts_with_dont(words_b[i]))
            neg_b++;
    }

    /* Count shared non-negation content words (skip very short words). */
    size_t shared = 0;
    size_t content_a = 0;
    for (size_t i = 0; i < na; i++) {
        if (strlen(words_a[i]) < 3)
            continue;
        if (is_negation(words_a[i]) || starts_with_dont(words_a[i]))
            continue;
        content_a++;
        for (size_t j = 0; j < nb; j++) {
            if (strcmp(words_a[i], words_b[j]) == 0) {
                shared++;
                break;
            }
        }
    }
    size_t content_b = 0;
    for (size_t i = 0; i < nb; i++) {
        if (strlen(words_b[i]) < 3)
            continue;
        if (is_negation(words_b[i]) || starts_with_dont(words_b[i]))
            continue;
        content_b++;
    }

    size_t content_max = content_a > content_b ? content_a : content_b;
    if (content_max == 0)
        return HU_BELIEF_CONFLICT_NONE;

    /* Negation asymmetry with shared root content → contradiction.
     * One text negates while the other doesn't, and they share enough
     * content words to be about the same subject. */
    bool neg_asym = (neg_a > 0) != (neg_b > 0);
    float overlap = (float)shared / (float)content_max;

    if (neg_asym && overlap > 0.3f)
        return HU_BELIEF_CONFLICT_CONTRADICT;

    /* >60% word overlap → paraphrase (same fact, different wording). */
    if (overlap > 0.6f)
        return HU_BELIEF_CONFLICT_PARAPHRASE;

    return HU_BELIEF_CONFLICT_NONE;
}

/* ── LLM-judge conflict detection ──────────────────────────────────── */

#if !(defined(HU_IS_TEST) && HU_IS_TEST)

static const char *const CONFLICT_JUDGE_SYSTEM =
    "You are a semantic conflict detector. Given two statements about a "
    "person, determine if they contradict each other. Reply with ONLY one "
    "word: CONFLICT, PARTIAL, or NONE.";

static hu_belief_conflict_t llm_judge_conflict(
    const char *a, size_t a_len,
    const char *b, size_t b_len,
    hu_provider_t *provider,
    hu_allocator_t *alloc) {
    if (!provider || !provider->vtable ||
        !provider->vtable->chat_with_system || !alloc)
        return hu_belief_semantic_conflict(a, a_len, b, b_len);

    char prompt[2048];
    int n = snprintf(prompt, sizeof(prompt),
                     "Statement A: %.*s\n\nStatement B: %.*s",
                     (int)(a_len > 900 ? 900 : a_len), a,
                     (int)(b_len > 900 ? 900 : b_len), b);
    if (n <= 0 || (size_t)n >= sizeof(prompt))
        return hu_belief_semantic_conflict(a, a_len, b, b_len);

    char *resp = NULL;
    size_t resp_len = 0;
    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc,
        CONFLICT_JUDGE_SYSTEM, strlen(CONFLICT_JUDGE_SYSTEM),
        prompt, (size_t)n,
        "", 0,
        0.0, &resp, &resp_len);

    if (err != HU_OK || !resp)
        return hu_belief_semantic_conflict(a, a_len, b, b_len);

    /* Strip leading whitespace from response. */
    const char *p = resp;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;

    hu_belief_conflict_t result;
    if (strncmp(p, "CONFLICT", 8) == 0)
        result = HU_BELIEF_CONFLICT_CONTRADICT;
    else if (strncmp(p, "PARTIAL", 7) == 0)
        result = HU_BELIEF_CONFLICT_PARAPHRASE;
    else if (strncmp(p, "NONE", 4) == 0)
        result = HU_BELIEF_CONFLICT_NONE;
    else
        result = hu_belief_semantic_conflict(a, a_len, b, b_len);

    alloc->free(alloc->ctx, resp, resp_len + 1);
    return result;
}

#endif /* !HU_IS_TEST */

hu_belief_conflict_t hu_belief_semantic_conflict_with_provider(
    const char *a, size_t a_len,
    const char *b, size_t b_len,
    hu_provider_t *provider,
    hu_allocator_t *alloc) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)alloc;
    return hu_belief_semantic_conflict(a, a_len, b, b_len);
#else
    if (!provider || !alloc)
        return hu_belief_semantic_conflict(a, a_len, b, b_len);
    return llm_judge_conflict(a, a_len, b, b_len, provider, alloc);
#endif
}
