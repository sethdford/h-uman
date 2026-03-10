#include "human/memory/fast_capture.h"
#include "human/core/string.h"
#include "human/data/loader.h"
#include "human/core/json.h"
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

/* Default fallback: relationship words (NULL-terminated) */
static const char *DEFAULT_RELATIONSHIP_WORDS[] = {
    "mom",      "dad",     "wife", "husband",  "friend",    "sister", "brother", "boss",
    "coworker", "partner", "son",  "daughter", "child",     "family", "manager", "mommy",
    "daddy",    "kid",     "kids", "spouse",   "colleague", NULL};

/* Default fallback: emotion adjectives */
struct emotion_adj {
    const char *word;
    size_t word_len;
    hu_emotion_tag_t tag;
};

static const struct emotion_adj DEFAULT_EMOTION_ADJECTIVES[] = {
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

/* Runtime loaded data */
static const char **s_relationship_words = (const char **)DEFAULT_RELATIONSHIP_WORDS;
static struct emotion_adj *s_emotion_adjectives = (struct emotion_adj *)DEFAULT_EMOTION_ADJECTIVES;
static size_t s_emotion_adj_count = 20;

/* Emotion tag map: string name -> hu_emotion_tag_t */
static hu_emotion_tag_t emotion_tag_from_string(const char *str) {
    if (!str) return 0;
    if (strcmp(str, "joy") == 0) return HU_EMOTION_JOY;
    if (strcmp(str, "excitement") == 0) return HU_EMOTION_EXCITEMENT;
    if (strcmp(str, "sadness") == 0) return HU_EMOTION_SADNESS;
    if (strcmp(str, "anger") == 0) return HU_EMOTION_ANGER;
    if (strcmp(str, "fear") == 0) return HU_EMOTION_FEAR;
    if (strcmp(str, "frustration") == 0) return HU_EMOTION_FRUSTRATION;
    if (strcmp(str, "anxiety") == 0) return HU_EMOTION_ANXIETY;
    return 0;
}

hu_error_t hu_fast_capture_data_init(hu_allocator_t *alloc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    /* Load relationship words */
    char *json_rel = NULL;
    size_t json_rel_len = 0;
    hu_error_t err = hu_data_load(alloc, "memory/relationship_words.json", &json_rel, &json_rel_len);
    if (err == HU_OK && json_rel) {
        hu_json_value_t *root_rel = NULL;
        err = hu_json_parse(alloc, json_rel, json_rel_len, &root_rel);
        alloc->free(alloc->ctx, json_rel, json_rel_len);
        if (err == HU_OK && root_rel) {
            hu_json_value_t *words_arr = hu_json_object_get(root_rel, "words");
            if (words_arr && words_arr->type == HU_JSON_ARRAY) {
                size_t count = words_arr->data.array.len;
                if (count > 0) {
                    const char **words = (const char **)alloc->alloc(alloc->ctx, (count + 1) * sizeof(const char *));
                    if (words) {
                        memset(words, 0, (count + 1) * sizeof(const char *));
                        for (size_t i = 0; i < count; i++) {
                            hu_json_value_t *item = words_arr->data.array.items[i];
                            if (item && item->type == HU_JSON_STRING) {
                                words[i] = hu_strndup(alloc, item->data.string.ptr, item->data.string.len);
                            }
                        }
                        words[count] = NULL;
                        s_relationship_words = words;
                    }
                }
            }
            hu_json_free(alloc, root_rel);
        }
    }

    /* Load emotion adjectives */
    char *json_emo = NULL;
    size_t json_emo_len = 0;
    err = hu_data_load(alloc, "memory/emotion_adjectives.json", &json_emo, &json_emo_len);
    if (err == HU_OK && json_emo) {
        hu_json_value_t *root_emo = NULL;
        err = hu_json_parse(alloc, json_emo, json_emo_len, &root_emo);
        alloc->free(alloc->ctx, json_emo, json_emo_len);
        if (err == HU_OK && root_emo) {
            hu_json_value_t *adj_arr = hu_json_object_get(root_emo, "adjectives");
            if (adj_arr && adj_arr->type == HU_JSON_ARRAY) {
                size_t count = adj_arr->data.array.len;
                if (count > 0) {
                    struct emotion_adj *adjs = (struct emotion_adj *)alloc->alloc(alloc->ctx, (count + 1) * sizeof(struct emotion_adj));
                    if (adjs) {
                        memset(adjs, 0, (count + 1) * sizeof(struct emotion_adj));
                        for (size_t i = 0; i < count; i++) {
                            hu_json_value_t *item = adj_arr->data.array.items[i];
                            if (item && item->type == HU_JSON_OBJECT) {
                                const char *word = hu_json_get_string(item, "word");
                                const char *emotion = hu_json_get_string(item, "emotion");
                                if (word) {
                                    adjs[i].word = hu_strndup(alloc, word, strlen(word));
                                    adjs[i].word_len = strlen(adjs[i].word);
                                    adjs[i].tag = emotion_tag_from_string(emotion);
                                }
                            }
                        }
                        adjs[count].word = NULL;
                        adjs[count].word_len = 0;
                        adjs[count].tag = 0;
                        s_emotion_adjectives = adjs;
                        s_emotion_adj_count = count;
                    }
                }
            }
            hu_json_free(alloc, root_emo);
        }
    }

    return HU_OK;
}

void hu_fast_capture_data_cleanup(hu_allocator_t *alloc) {
    if (!alloc)
        return;

    /* Free relationship words if not default */
    if (s_relationship_words != (const char **)DEFAULT_RELATIONSHIP_WORDS) {
        for (size_t i = 0; s_relationship_words[i]; i++) {
            alloc->free(alloc->ctx, (char *)s_relationship_words[i], strlen(s_relationship_words[i]) + 1);
        }
        size_t count = 0;
        for (size_t i = 0; s_relationship_words[i]; i++) count++;
        alloc->free(alloc->ctx, s_relationship_words, (count + 1) * sizeof(const char *));
    }

    /* Free emotion adjectives if not default */
    if (s_emotion_adjectives != (struct emotion_adj *)DEFAULT_EMOTION_ADJECTIVES) {
        for (size_t i = 0; i < s_emotion_adj_count; i++) {
            if (s_emotion_adjectives[i].word) {
                alloc->free(alloc->ctx, (char *)s_emotion_adjectives[i].word, strlen(s_emotion_adjectives[i].word) + 1);
            }
        }
        alloc->free(alloc->ctx, s_emotion_adjectives, (s_emotion_adj_count + 1) * sizeof(struct emotion_adj));
    }

    s_relationship_words = (const char **)DEFAULT_RELATIONSHIP_WORDS;
    s_emotion_adjectives = (struct emotion_adj *)DEFAULT_EMOTION_ADJECTIVES;
    s_emotion_adj_count = 20;
}

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
        for (size_t r = 0; s_relationship_words[r]; r++) {
            size_t rlen = strlen(s_relationship_words[r]);
            if (word_len == rlen &&
                fc_strncasecmp(text + word_start, s_relationship_words[r], rlen) == 0) {
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
    static const char *EMOTION_PREFIXES[] = {"I feel ", "I'm feeling ", "I am ", "I'm ", NULL};
    for (size_t p = 0; EMOTION_PREFIXES[p]; p++) {
        const char *prefix = EMOTION_PREFIXES[p];
        size_t plen = strlen(prefix);
        const char *found = fc_strstr_case(text, text_len, prefix, plen);
        if (!found)
            continue;
        size_t rest_start = (size_t)(found - text) + plen;
        size_t rest_len = text_len - rest_start;
        if (rest_len == 0)
            continue;
        for (size_t a = 0; a < s_emotion_adj_count; a++) {
            const char *adj = s_emotion_adjectives[a].word;
            size_t alen = s_emotion_adjectives[a].word_len;
            if (adj && fc_strstr_case(text + rest_start, rest_len, adj, alen))
                add_emotion(out, s_emotion_adjectives[a].tag, 0.8);
        }
    }
}

/* Structural: match prefix+word or standalone word for topic. */
static void scan_topics(const char *text, size_t text_len, hu_fc_result_t *out,
                        hu_allocator_t *alloc) {
    if (out->primary_topic)
        return;
    for (size_t t = 0; TOPIC_PATTERNS[t].topic; t++) {
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
    for (size_t i = 0; COMMITMENT_PREFIXES[i]; i++) {
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
