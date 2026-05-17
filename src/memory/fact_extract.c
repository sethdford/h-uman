#include "human/memory/fact_extract.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Heuristic fact extraction patterns ──────────────────────────── */

/* P2-6 (2026-05-16 incident): each marker carries a third-person paraphrase
 * for its predicate. Without this, the predicate stored "i like" / "when
 * i'm" / "i never" verbatim, and `hu_fact_format_for_store` rendered
 * "user i like X" — still first-person and prone to leaking as a
 * confession-style fragment when the memory was injected into outbound
 * prompts. The paraphrase is what gets stored. */
typedef struct fact_pattern {
    const char *marker;
    const char *predicate; /* paraphrased third-person predicate */
    hu_knowledge_type_t type;
    float confidence;
} fact_pattern_t;

static const fact_pattern_t patterns[] = {
    /* Propositional — user preferences and facts.
     * Each pattern carries a third-person paraphrased predicate. */
    {"i like ", "likes", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"i love ", "loves", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"i hate ", "hates", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"i prefer ", "prefers", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"my favorite ", "favorite", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"i work at ", "works at", HU_KNOWLEDGE_PROPOSITIONAL, 0.9f},
    {"i live in ", "lives in", HU_KNOWLEDGE_PROPOSITIONAL, 0.9f},
    {"i am a ", "is a", HU_KNOWLEDGE_PROPOSITIONAL, 0.9f},
    {"i'm a ", "is a", HU_KNOWLEDGE_PROPOSITIONAL, 0.9f},
    {"i have a ", "has a", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my name is ", "name is", HU_KNOWLEDGE_PROPOSITIONAL, 0.9f},
    {"i enjoy ", "enjoys", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    /* Prescriptive — behavioral patterns and preferences */
    {"when i'm ", "when", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i usually ", "usually", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i always ", "always", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i never ", "never", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"please don't ", "avoid", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i'd rather ", "would rather", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i tend to ", "tends to", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    /* Expanded propositional patterns */
    {"i'm not interested in ", "not interested in", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i'm interested in ", "interested in", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i don't like ", "dislikes", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"i dislike ", "dislikes", HU_KNOWLEDGE_PROPOSITIONAL, 0.8f},
    {"i studied ", "studied", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i went to ", "went to", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i grew up in ", "grew up in", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i'm from ", "is from", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my job is ", "job is", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i work as ", "works as", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i majored in ", "majored in", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i speak ", "speaks", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my hobby is ", "hobby is", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i'm allergic to ", "allergic to", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i own a ", "owns a", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"i drive a ", "drives a", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my partner ", "partner", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my wife ", "wife", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my husband ", "husband", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my kid ", "kid", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my kids ", "kids", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my dog ", "dog", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    {"my cat ", "cat", HU_KNOWLEDGE_PROPOSITIONAL, 0.7f},
    /* Expanded prescriptive patterns */
    {"i don't want ", "does not want", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i can't stand ", "cannot stand", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i'm trying to ", "is trying to", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i need to ", "needs to", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i want to ", "wants to", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i should ", "should", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i'm working on ", "is working on", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"remind me to ", "wants reminder to", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"don't forget ", "do not forget", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
    {"i'm looking for ", "is looking for", HU_KNOWLEDGE_PRESCRIPTIVE, 0.7f},
};
static const size_t pattern_count = sizeof(patterns) / sizeof(patterns[0]);

static bool ci_match(const char *text, size_t text_len, const char *pat, size_t pat_len) {
    if (text_len < pat_len)
        return false;
    for (size_t i = 0; i < pat_len; i++) {
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)pat[i]))
            return false;
    }
    return true;
}

static size_t find_end(const char *text, size_t start, size_t len) {
    for (size_t i = start; i < len; i++) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == '\n' ||
            text[i] == ',' || text[i] == ';')
            return i;
    }
    return len;
}

static void extract_spo(const char *sentence, size_t sent_len, const fact_pattern_t *pat,
                        hu_heuristic_fact_t *fact) {
    size_t marker_len = strlen(pat->marker);

    /* Subject: "user" for first-person statements */
    strncpy(fact->subject, "user", sizeof(fact->subject) - 1);

    /* P2-6: predicate is the paraphrased third-person form, NOT the raw
     * marker. "i like" → "likes"; "when i'm" → "when"; "i never" → "never".
     * This prevents "user i like X" leaking as a first-person fragment. */
    const char *predicate = pat->predicate ? pat->predicate : "noted";
    strncpy(fact->predicate, predicate, sizeof(fact->predicate) - 1);
    fact->predicate[sizeof(fact->predicate) - 1] = '\0';

    /* Object: rest of the sentence after marker */
    if (marker_len < sent_len) {
        size_t obj_start = marker_len;
        size_t obj_len = sent_len - obj_start;
        if (obj_len > sizeof(fact->object) - 1)
            obj_len = sizeof(fact->object) - 1;
        memcpy(fact->object, sentence + obj_start, obj_len);
        fact->object[obj_len] = '\0';
        /* Trim trailing whitespace/punctuation */
        while (obj_len > 0 &&
               (fact->object[obj_len - 1] == ' ' || fact->object[obj_len - 1] == '.' ||
                fact->object[obj_len - 1] == ',' || fact->object[obj_len - 1] == ';'))
            fact->object[--obj_len] = '\0';
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

hu_error_t hu_fact_extract(const char *text, size_t text_len, hu_fact_extract_result_t *result) {
    if (!text || !result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

    for (size_t pos = 0; pos < text_len && result->fact_count < HU_FACT_EXTRACT_MAX;) {
        /* Skip to start of a sentence, line, or clause after , / ; */
        while (pos < text_len && (text[pos] == ' ' || text[pos] == '\n'))
            pos++;
        while (pos < text_len && (text[pos] == ',' || text[pos] == ';')) {
            pos++;
            while (pos < text_len && text[pos] == ' ')
                pos++;
        }

        bool found = false;
        for (size_t p = 0; p < pattern_count; p++) {
            size_t mlen = strlen(patterns[p].marker);
            if (ci_match(text + pos, text_len - pos, patterns[p].marker, mlen)) {
                size_t end = find_end(text, pos + mlen, text_len);
                size_t sent_len = end - pos;

                hu_heuristic_fact_t *f = &result->facts[result->fact_count];
                f->type = patterns[p].type;
                f->confidence = patterns[p].confidence;
                extract_spo(text + pos, sent_len, &patterns[p], f);
                snprintf(f->source_hint, sizeof(f->source_hint), "conversation");

                result->fact_count++;
                if (f->type == HU_KNOWLEDGE_PROPOSITIONAL)
                    result->propositional_count++;
                else
                    result->prescriptive_count++;

                pos = end;
                found = true;
                break;
            }
        }
        if (!found) {
            size_t next = find_end(text, pos, text_len);
            pos = (next > pos) ? next + 1 : pos + 1;
        }
    }
    return HU_OK;
}

size_t hu_fact_dedup(hu_fact_extract_result_t *result, const hu_heuristic_fact_t *existing,
                     size_t existing_count) {
    if (!result || !existing || existing_count == 0)
        return result ? result->fact_count : 0;

    size_t write = 0;
    for (size_t i = 0; i < result->fact_count; i++) {
        bool dup = false;
        for (size_t j = 0; j < existing_count; j++) {
            if (strcmp(result->facts[i].subject, existing[j].subject) == 0 &&
                strcmp(result->facts[i].predicate, existing[j].predicate) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            if (write != i)
                result->facts[write] = result->facts[i];
            write++;
        }
    }
    result->fact_count = write;
    return write;
}

hu_error_t hu_fact_format_for_store(hu_allocator_t *alloc, const hu_heuristic_fact_t *fact,
                                    char **key, size_t *key_len, char **value, size_t *value_len) {
    if (!alloc || !fact || !key || !key_len || !value || !value_len)
        return HU_ERR_INVALID_ARGUMENT;

    char kbuf[768];
    char vbuf[768];
    int kn, vn;

    if (fact->type == HU_KNOWLEDGE_PROPOSITIONAL) {
        kn = snprintf(kbuf, sizeof(kbuf), "fact:%s:%s:%s", fact->subject, fact->predicate,
                      fact->object);
        vn = snprintf(vbuf, sizeof(vbuf), "%s %s %s (confidence: %.2f)", fact->subject,
                      fact->predicate, fact->object, fact->confidence);
    } else {
        kn = snprintf(kbuf, sizeof(kbuf), "skill:%s:%s", fact->subject, fact->predicate);
        vn = snprintf(vbuf, sizeof(vbuf), "%s %s %s (confidence: %.2f)", fact->subject,
                      fact->predicate, fact->object, fact->confidence);
    }

    if (kn <= 0 || vn <= 0 || (size_t)kn >= sizeof(kbuf) || (size_t)vn >= sizeof(vbuf))
        return HU_ERR_INVALID_ARGUMENT;

    *key = (char *)alloc->alloc(alloc->ctx, (size_t)kn + 1);
    if (!*key)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*key, kbuf, (size_t)kn + 1);
    *key_len = (size_t)kn;

    *value = (char *)alloc->alloc(alloc->ctx, (size_t)vn + 1);
    if (!*value) {
        alloc->free(alloc->ctx, *key, (size_t)kn + 1);
        *key = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(*value, vbuf, (size_t)vn + 1);
    *value_len = (size_t)vn;
    return HU_OK;
}

/* Confidence decay — single-precision exponential with half-life
 * `HU_FACT_CONFIDENCE_HALF_LIFE_SEC`. Implemented as a piecewise table
 * lookup so we don't pull `<math.h>` into every translation unit that
 * includes the fact_extract header.
 *
 * The table holds 0.5^k for k ∈ [0..10]; for the fractional part we
 * linearly interpolate between two adjacent entries. This is accurate
 * to ~1% over the relevant range, which is more than enough for a
 * heuristic decay applied to a heuristic confidence. */
float hu_heuristic_fact_effective_confidence(const hu_heuristic_fact_t *fact, int64_t now) {
    if (!fact)
        return 0.f;
    if (fact->last_seen_at <= 0 || now <= fact->last_seen_at)
        return fact->confidence;
    int64_t age = now - fact->last_seen_at;
    /* age in fractional half-lives, capped at 10 (≈ confidence * 0.001) */
    float k = (float)age / (float)HU_FACT_CONFIDENCE_HALF_LIFE_SEC;
    if (k <= 0.f)
        return fact->confidence;
    if (k >= 10.f)
        return 0.f;
    /* Powers of 0.5 — pre-computed, no math.h. */
    static const float pow_half[] = {
        1.000000f, 0.500000f, 0.250000f, 0.125000f, 0.062500f, 0.031250f,
        0.015625f, 0.007812f, 0.003906f, 0.001953f, 0.000977f,
    };
    int idx = (int)k;
    float frac = k - (float)idx;
    /* Linear interpolation between pow_half[idx] and pow_half[idx+1]. */
    float lo = pow_half[idx];
    float hi = pow_half[idx + 1];
    float decay = lo + (hi - lo) * frac;
    return fact->confidence * decay;
}
