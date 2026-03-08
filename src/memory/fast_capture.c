#include "seaclaw/memory/fast_capture.h"
#include "seaclaw/core/string.h"
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

static const char *RELATIONSHIP_PATTERNS[] = {
    "my mom",     "my dad",    "my wife",     "my husband", "my friend", "my sister",
    "my brother", "my boss",   "my coworker", "my partner", "my son",    "my daughter",
    "my child",   "my family", "my manager",  NULL};

static const char *EMOTION_JOY_PATTERNS[] = {"I feel great", "I'm so happy", "I'm excited",
                                             "amazing",      "wonderful",    NULL};
static const char *EMOTION_SAD_PATTERNS[] = {"I feel sad", "I'm depressed", "I'm down",
                                             "heartbroken", NULL};
static const char *EMOTION_ANGER_PATTERNS[] = {"I'm so angry", "I'm furious", "pissed off",
                                               "infuriating", NULL};
static const char *EMOTION_FEAR_PATTERNS[] = {"I'm scared", "I'm afraid", "terrified", "frightened",
                                              NULL};
static const char *EMOTION_FRUSTRATION_PATTERNS[] = {
    "I'm frustrated", "I'm so frustrated", "so frustrating", "I can't believe", "fed up", NULL};
static const char *EMOTION_ANXIETY_PATTERNS[] = {
    "I'm anxious", "I'm worried", "I'm stressed", "can't stop thinking", "anxious", NULL};
static const char *EMOTION_EXCITEMENT_PATTERNS[] = {"I'm excited", "so excited", NULL};

static const char *TOPIC_WORK_PATTERNS[] = {"at work",     "my job",   "the office", "my boss",
                                            "the project", "deadline", NULL};
static const char *TOPIC_HEALTH_PATTERNS[] = {"the doctor",   "my health",     "feeling sick",
                                              "the hospital", "my medication", NULL};
static const char *TOPIC_RELATIONSHIP_PATTERNS[] = {"my relationship", "my partner", "we broke up",
                                                    "dating", NULL};
static const char *TOPIC_FINANCE_PATTERNS[] = {"my budget", "savings", "rent", "bills",
                                               "debt",      "salary",  NULL};

static const char *COMMITMENT_PATTERNS[] = {
    "I will ", "I'll ", "I promise", "I'm going to ", "I plan to ", "remind me to ", NULL};

static void add_entity(sc_fc_result_t *out, sc_allocator_t *alloc, const char *name,
                       size_t name_len, const char *type, size_t type_len, double confidence,
                       size_t offset) {
    if (out->entity_count >= SC_FC_MAX_RESULTS)
        return;
    sc_fc_entity_match_t *e = &out->entities[out->entity_count];
    e->name = sc_strndup(alloc, name, name_len);
    e->name_len = name_len;
    e->type = sc_strndup(alloc, type, type_len);
    e->type_len = type_len;
    e->confidence = confidence;
    e->offset = offset;
    out->entity_count++;
}

static void add_emotion(sc_fc_result_t *out, sc_emotion_tag_t tag, double intensity) {
    if (out->emotion_count >= SC_STM_MAX_EMOTIONS)
        return;
    for (size_t i = 0; i < out->emotion_count; i++) {
        if (out->emotions[i].tag == tag)
            return;
    }
    out->emotions[out->emotion_count].tag = tag;
    out->emotions[out->emotion_count].intensity = intensity;
    out->emotion_count++;
}

static void scan_relationships(const char *text, size_t text_len, sc_fc_result_t *out,
                               sc_allocator_t *alloc) {
    for (int i = 0; RELATIONSHIP_PATTERNS[i]; i++) {
        const char *pat = RELATIONSHIP_PATTERNS[i];
        size_t plen = strlen(pat);
        const char *p = fc_strstr_case(text, text_len, pat, plen);
        if (!p)
            continue;
        size_t offset = (size_t)(p - text);
        /* "my X" -> extract X (e.g. "mom" from "my mom") */
        if (plen >= 4 && fc_strncasecmp(pat, "my ", 3) == 0) {
            const char *name = p + 3;
            size_t name_len = plen - 3;
            add_entity(out, alloc, name, name_len, "person", 6, 0.85, offset);
        }
    }
}

static void scan_emotions(const char *text, size_t text_len, sc_fc_result_t *out) {
    struct {
        const char **pats;
        sc_emotion_tag_t tag;
    } maps[] = {
        {EMOTION_JOY_PATTERNS, SC_EMOTION_JOY},
        {EMOTION_SAD_PATTERNS, SC_EMOTION_SADNESS},
        {EMOTION_ANGER_PATTERNS, SC_EMOTION_ANGER},
        {EMOTION_FEAR_PATTERNS, SC_EMOTION_FEAR},
        {EMOTION_FRUSTRATION_PATTERNS, SC_EMOTION_FRUSTRATION},
        {EMOTION_ANXIETY_PATTERNS, SC_EMOTION_ANXIETY},
        {EMOTION_EXCITEMENT_PATTERNS, SC_EMOTION_EXCITEMENT},
    };
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        for (int i = 0; maps[m].pats[i]; i++) {
            const char *pat = maps[m].pats[i];
            size_t plen = strlen(pat);
            if (fc_strstr_case(text, text_len, pat, plen))
                add_emotion(out, maps[m].tag, 0.8);
        }
    }
}

static void scan_topics(const char *text, size_t text_len, sc_fc_result_t *out,
                        sc_allocator_t *alloc) {
    if (out->primary_topic)
        return;
    struct {
        const char **pats;
        const char *name;
        size_t name_len;
    } maps[] = {
        {TOPIC_WORK_PATTERNS, "work", 4},
        {TOPIC_HEALTH_PATTERNS, "health", 6},
        {TOPIC_RELATIONSHIP_PATTERNS, "relationship", 12},
        {TOPIC_FINANCE_PATTERNS, "finance", 7},
    };
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        for (int i = 0; maps[m].pats[i]; i++) {
            const char *pat = maps[m].pats[i];
            size_t plen = strlen(pat);
            if (fc_strstr_case(text, text_len, pat, plen)) {
                out->primary_topic = sc_strndup(alloc, maps[m].name, maps[m].name_len);
                return;
            }
        }
    }
}

static void scan_commitments(const char *text, size_t text_len, sc_fc_result_t *out) {
    for (int i = 0; COMMITMENT_PATTERNS[i]; i++) {
        const char *pat = COMMITMENT_PATTERNS[i];
        size_t plen = strlen(pat);
        if (fc_strstr_case(text, text_len, pat, plen)) {
            out->has_commitment = true;
            return;
        }
    }
}

static void scan_question(const char *text, size_t text_len, sc_fc_result_t *out) {
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '?') {
            out->has_question = true;
            return;
        }
    }
}

sc_error_t sc_fast_capture(sc_allocator_t *alloc, const char *text, size_t text_len,
                           sc_fc_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!text || text_len == 0)
        return SC_OK;

    scan_relationships(text, text_len, out, alloc);
    scan_emotions(text, text_len, out);
    scan_topics(text, text_len, out, alloc);
    scan_commitments(text, text_len, out);
    scan_question(text, text_len, out);
    return SC_OK;
}

void sc_fc_result_deinit(sc_fc_result_t *result, sc_allocator_t *alloc) {
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
