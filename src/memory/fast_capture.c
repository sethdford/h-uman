#include "human/memory/fast_capture.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int fc_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb))
            return ca - cb;
        if (ca == '\0')
            return 0;
    }
    return 0;
}

static const char *fc_strstr_case(const char *haystack, size_t hay_len, const char *needle,
                                  size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len)
        return NULL;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (fc_strncasecmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Structural: "my " + relationship word. Broader than fixed phrases. */
static const char *RELATIONSHIP_WORDS[] = {
    "mom",      "dad",     "wife", "husband",  "friend",    "sister", "brother", "boss",
    "coworker", "partner", "son",  "daughter", "child",     "family", "manager", "mommy",
    "daddy",    "kid",     "kids", "spouse",   "colleague", NULL};

/* Structural: "I feel/I'm feeling/I am/I'm " + adjective. Adjectives map to emotion. */
static const char *EMOTION_PREFIXES[] = {"I feel ", "I'm feeling ", "I am ", "I'm ", NULL};
struct emotion_adj {
    const char *word;
    size_t word_len;
    hu_emotion_tag_t tag;
};
static const struct emotion_adj EMOTION_ADJECTIVES[] = {
    {"great", 5, HU_EMOTION_JOY},
    {"happy", 5, HU_EMOTION_JOY},
    {"excited", 7, HU_EMOTION_EXCITEMENT},
    {"wonderful", 9, HU_EMOTION_JOY},
    {"amazing", 7, HU_EMOTION_JOY},
    {"sad", 3, HU_EMOTION_SADNESS},
    {"depressed", 9, HU_EMOTION_SADNESS},
    {"down", 4, HU_EMOTION_SADNESS},
    {"heartbroken", 11, HU_EMOTION_SADNESS},
    {"angry", 5, HU_EMOTION_ANGER},
    {"furious", 7, HU_EMOTION_ANGER},
    {"scared", 6, HU_EMOTION_FEAR},
    {"afraid", 6, HU_EMOTION_FEAR},
    {"terrified", 9, HU_EMOTION_FEAR},
    {"frightened", 10, HU_EMOTION_FEAR},
    {"frustrated", 10, HU_EMOTION_FRUSTRATION},
    {"frustrating", 11, HU_EMOTION_FRUSTRATION},
    {"anxious", 7, HU_EMOTION_ANXIETY},
    {"worried", 7, HU_EMOTION_ANXIETY},
    {"stressed", 8, HU_EMOTION_ANXIETY},
    {NULL, 0, 0},
};

/* Structural: "at/the/about/my " + topic word, or standalone topic word. */
struct topic_pattern {
    const char *prefix;
    size_t prefix_len;
    const char *word;
    size_t word_len;
    const char *topic;
    size_t topic_len;
};
static const struct topic_pattern TOPIC_PATTERNS[] = {
    {"at ", 3, "work", 4, "work", 4},
    {"at the ", 7, "office", 6, "work", 4},
    {"my ", 3, "job", 3, "work", 4},
    {"the ", 4, "office", 6, "work", 4},
    {"the ", 4, "project", 7, "work", 4},
    {"my ", 3, "boss", 4, "work", 4},
    {NULL, 0, "deadline", 8, "work", 4},
    {"the ", 4, "doctor", 6, "health", 6},
    {"my ", 3, "health", 6, "health", 6},
    {"the ", 4, "hospital", 8, "health", 6},
    {"my ", 3, "medication", 10, "health", 6},
    {"feeling ", 8, "sick", 4, "health", 6},
    {"my ", 3, "relationship", 12, "relationship", 12},
    {"my ", 3, "partner", 7, "relationship", 12},
    {NULL, 0, "dating", 6, "relationship", 12},
    {"my ", 3, "budget", 6, "finance", 7},
    {NULL, 0, "savings", 7, "finance", 7},
    {NULL, 0, "rent", 4, "finance", 7},
    {NULL, 0, "bills", 5, "finance", 7},
    {NULL, 0, "debt", 4, "finance", 7},
    {NULL, 0, "salary", 6, "finance", 7},
    {NULL, 0, NULL, 0, NULL, 0},
};

/* Structural: commitment prefix patterns. */
static const char *COMMITMENT_PREFIXES[] = {"I will ",       "I'll ",      "I promise",
                                            "I'm going to ", "I plan to ", "remind me to ",
                                            "I'm gonna ",    NULL};

static void add_entity(hu_fc_result_t *out, hu_allocator_t *alloc, const char *name,
                       size_t name_len, const char *type, size_t type_len, double confidence,
                       size_t offset) {
    if (out->entity_count >= HU_FC_MAX_RESULTS)
        return;
    hu_fc_entity_match_t *e = &out->entities[out->entity_count];
    e->name = hu_strndup(alloc, name, name_len);
    if (!e->name)
        return;
    e->name_len = name_len;
    e->type = hu_strndup(alloc, type, type_len);
    if (!e->type) {
        alloc->free(alloc->ctx, e->name, name_len + 1);
        e->name = NULL;
        return;
    }
    e->type_len = type_len;
    e->confidence = confidence;
    e->offset = offset;
    out->entity_count++;
}

static void add_emotion(hu_fc_result_t *out, hu_emotion_tag_t tag, double intensity) {
    if (out->emotion_count >= HU_STM_MAX_EMOTIONS)
        return;
    for (size_t i = 0; i < out->emotion_count; i++) {
        if (out->emotions[i].tag == tag)
            return;
    }
    out->emotions[out->emotion_count].tag = tag;
    out->emotions[out->emotion_count].intensity = intensity;
    out->emotion_count++;
}

/* Structural: find "my " then check if next word is a relationship word. */
static void scan_relationships(const char *text, size_t text_len, hu_fc_result_t *out,
                               hu_allocator_t *alloc) {
    const char *prefix = "my ";
    size_t prefix_len = 3;
    if (text_len < prefix_len + 1)
        return;
    for (size_t i = 0; i <= text_len - prefix_len; i++) {
        if (fc_strncasecmp(text + i, prefix, prefix_len) != 0)
            continue;
        size_t word_start = i + prefix_len;
        size_t word_end = word_start;
        while (word_end < text_len &&
               (isalnum((unsigned char)text[word_end]) || text[word_end] == '\''))
            word_end++;
        size_t word_len = word_end - word_start;
        if (word_len == 0)
            continue;
        for (int r = 0; RELATIONSHIP_WORDS[r]; r++) {
            size_t rlen = strlen(RELATIONSHIP_WORDS[r]);
            if (word_len == rlen &&
                fc_strncasecmp(text + word_start, RELATIONSHIP_WORDS[r], rlen) == 0) {
                add_entity(out, alloc, text + word_start, word_len, "person", 6, 0.85, i);
                return;
            }
        }
        /* "my " + capitalized word (e.g. "my Sarah") — likely a person */
        if (word_len >= 2 && word_len <= 24 && (unsigned char)text[word_start] >= 'A' &&
            (unsigned char)text[word_start] <= 'Z') {
            add_entity(out, alloc, text + word_start, word_len, "person", 6, 0.75, i);
            return;
        }
    }
}

/* Structural: find "I feel/I'm feeling/I am/I'm " then look for emotion adjective in remainder. */
static void scan_emotions(const char *text, size_t text_len, hu_fc_result_t *out) {
    for (int p = 0; EMOTION_PREFIXES[p]; p++) {
        const char *prefix = EMOTION_PREFIXES[p];
        size_t plen = strlen(prefix);
        const char *found = fc_strstr_case(text, text_len, prefix, plen);
        if (!found)
            continue;
        size_t rest_start = (size_t)(found - text) + plen;
        size_t rest_len = text_len - rest_start;
        if (rest_len == 0)
            continue;
        for (int a = 0; EMOTION_ADJECTIVES[a].word; a++) {
            const char *adj = EMOTION_ADJECTIVES[a].word;
            size_t alen = EMOTION_ADJECTIVES[a].word_len;
            if (fc_strstr_case(text + rest_start, rest_len, adj, alen))
                add_emotion(out, EMOTION_ADJECTIVES[a].tag, 0.8);
        }
    }
}

/* Structural: match prefix+word or standalone word for topic. */
static void scan_topics(const char *text, size_t text_len, hu_fc_result_t *out,
                        hu_allocator_t *alloc) {
    if (out->primary_topic)
        return;
    for (int t = 0; TOPIC_PATTERNS[t].topic; t++) {
        const struct topic_pattern *tp = &TOPIC_PATTERNS[t];
        if (tp->prefix) {
            if (text_len < tp->prefix_len + tp->word_len)
                continue;
            const char *p = fc_strstr_case(text, text_len, tp->prefix, tp->prefix_len);
            if (!p)
                continue;
            size_t after = (size_t)(p - text) + tp->prefix_len;
            if (after + tp->word_len <= text_len &&
                fc_strncasecmp(text + after, tp->word, tp->word_len) == 0) {
                int next = (int)after + (int)tp->word_len;
                if (next >= (int)text_len || !isalnum((unsigned char)text[(size_t)next])) {
                    out->primary_topic = hu_strndup(alloc, tp->topic, tp->topic_len);
                    return;
                }
            }
        } else {
            if (fc_strstr_case(text, text_len, tp->word, tp->word_len)) {
                out->primary_topic = hu_strndup(alloc, tp->topic, tp->topic_len);
                return;
            }
        }
    }
}

static void scan_commitments(const char *text, size_t text_len, hu_fc_result_t *out) {
    for (int i = 0; COMMITMENT_PREFIXES[i]; i++) {
        const char *pat = COMMITMENT_PREFIXES[i];
        size_t plen = strlen(pat);
        if (fc_strstr_case(text, text_len, pat, plen)) {
            out->has_commitment = true;
            return;
        }
    }
}

static void scan_question(const char *text, size_t text_len, hu_fc_result_t *out) {
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '?') {
            out->has_question = true;
            return;
        }
    }
}

hu_error_t hu_fast_capture(hu_allocator_t *alloc, const char *text, size_t text_len,
                           hu_fc_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!text || text_len == 0)
        return HU_OK;

    scan_relationships(text, text_len, out, alloc);
    scan_emotions(text, text_len, out);
    scan_topics(text, text_len, out, alloc);
    scan_commitments(text, text_len, out);
    scan_question(text, text_len, out);
    return HU_OK;
}

void hu_fc_result_deinit(hu_fc_result_t *result, hu_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->entity_count; i++) {
        if (result->entities[i].name)
            alloc->free(alloc->ctx, result->entities[i].name, result->entities[i].name_len + 1);
        if (result->entities[i].type)
            alloc->free(alloc->ctx, result->entities[i].type, result->entities[i].type_len + 1);
    }
    result->entity_count = 0;
    if (result->primary_topic) {
        alloc->free(alloc->ctx, result->primary_topic, strlen(result->primary_topic) + 1);
        result->primary_topic = NULL;
    }
}
