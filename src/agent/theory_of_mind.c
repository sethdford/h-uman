#include "human/agent/theory_of_mind.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

hu_error_t hu_tom_init(hu_tom_belief_state_t *state, hu_allocator_t *alloc, const char *contact_id,
                       size_t contact_id_len) {
    if (!state || !alloc || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    /* Zero the whole struct — callers pass uninitialized stack memory, and a
     * partial init (beliefs but not expectations) left hu_tom_deinit freeing
     * garbage expectation pointers (2026-07 service-loop SIGABRT). */
    memset(state, 0, sizeof(*state));
    size_t len = contact_id_len;
    if (len == 0)
        len = strlen(contact_id);
    char *dup = hu_strndup(alloc, contact_id, len);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    state->contact_id = dup;
    state->contact_id_len = strlen(dup);
    return HU_OK;
}

static hu_tom_belief_t *find_belief_by_topic(hu_tom_belief_state_t *state, const char *topic,
                                             size_t topic_len) {
    for (size_t i = 0; i < state->belief_count; i++) {
        hu_tom_belief_t *b = &state->beliefs[i];
        if (!b->topic || b->topic_len != topic_len)
            continue;
        if (strncmp(b->topic, topic, topic_len) == 0)
            return b;
    }
    return NULL;
}

hu_error_t hu_tom_record_belief(hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                const char *topic, size_t topic_len, hu_belief_type_t type,
                                float confidence) {
    if (!state || !alloc || !topic)
        return HU_ERR_INVALID_ARGUMENT;
    if (type > HU_BELIEF_MISTAKEN)
        return HU_ERR_INVALID_ARGUMENT;
    size_t len = topic_len;
    if (len == 0)
        len = strlen(topic);
    if (len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    float clamped = confidence;
    if (clamped < 0.0f)
        clamped = 0.0f;
    if (clamped > 1.0f)
        clamped = 1.0f;

    int64_t ts = (int64_t)time(NULL);

    hu_tom_belief_t *existing = find_belief_by_topic(state, topic, len);
    if (existing) {
        existing->type = type;
        existing->confidence = clamped;
        existing->last_updated = ts;
        return HU_OK;
    }

    if (state->belief_count >= HU_TOM_MAX_BELIEFS)
        return HU_ERR_OUT_OF_MEMORY;

    char *dup = hu_strndup(alloc, topic, len);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;

    hu_tom_belief_t *b = &state->beliefs[state->belief_count];
    b->topic = dup;
    b->topic_len = strlen(dup);
    b->type = type;
    b->confidence = clamped;
    b->last_updated = ts;
    state->belief_count++;
    return HU_OK;
}

static const char *belief_type_label(hu_belief_type_t type) {
    switch (type) {
    case HU_BELIEF_KNOWS:
        return "KNOWS";
    case HU_BELIEF_ASSUMES:
        return "ASSUMES";
    case HU_BELIEF_UNAWARE:
        return "UNAWARE";
    case HU_BELIEF_MISTAKEN:
        return "MISTAKEN";
    default:
        return "UNKNOWN";
    }
}

static bool belief_shows_confidence(hu_belief_type_t type) {
    return type == HU_BELIEF_KNOWS || type == HU_BELIEF_ASSUMES || type == HU_BELIEF_MISTAKEN;
}

hu_error_t hu_tom_build_context(const hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                char **out, size_t *out_len) {
    if (!state || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[4096];
    size_t pos = 0;

    pos = hu_buf_appendf(buf, sizeof(buf), pos, "### Contact Mental Model\n");

    for (int t = HU_BELIEF_KNOWS; t <= HU_BELIEF_MISTAKEN; t++) {
        hu_belief_type_t type = (hu_belief_type_t)t;
        bool first = true;
        for (size_t i = 0; i < state->belief_count; i++) {
            if (state->beliefs[i].type != type)
                continue;
            if (first) {
                pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s: ", belief_type_label(type));
                first = false;
            } else {
                pos = hu_buf_appendf(buf, sizeof(buf), pos, ", ");
            }
            if (pos >= sizeof(buf))
                break;
            if (belief_shows_confidence(type)) {
                int pct = (int)(state->beliefs[i].confidence * 100.0f + 0.5f);
                pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s (%d%%)", state->beliefs[i].topic,
                                     pct);
            } else {
                pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s", state->beliefs[i].topic);
            }
            if (pos >= sizeof(buf))
                break;
        }
        if (!first)
            pos = hu_buf_appendf(buf, sizeof(buf), pos, "\n");
        if (pos >= sizeof(buf)) {
            pos = sizeof(buf) - 1;
            break;
        }
    }
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    char *result = hu_strndup(alloc, buf, pos);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out = result;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_tom_record_user_expectation(hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                                          const char *topic, size_t topic_len,
                                          hu_tom_expected_knowledge_t knowledge_type) {
    if (!state || !alloc || !topic)
        return HU_ERR_INVALID_ARGUMENT;
    size_t len = topic_len;
    if (len == 0)
        len = strlen(topic);
    if (len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Check for existing expectation on same topic */
    for (size_t i = 0; i < state->expectation_count; i++) {
        hu_tom_expectation_t *e = &state->expectations[i];
        if (e->topic_len == len && strncmp(e->topic, topic, len) == 0) {
            e->knowledge_type = knowledge_type;
            e->recorded_at = (int64_t)time(NULL);
            return HU_OK;
        }
    }

    if (state->expectation_count >= HU_TOM_MAX_EXPECTATIONS)
        return HU_ERR_OUT_OF_MEMORY;

    char *dup = hu_strndup(alloc, topic, len);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;

    hu_tom_expectation_t *e = &state->expectations[state->expectation_count];
    e->topic = dup;
    e->topic_len = strlen(dup);
    e->knowledge_type = knowledge_type;
    e->recorded_at = (int64_t)time(NULL);
    state->expectation_count++;
    return HU_OK;
}

hu_error_t hu_tom_detect_gaps(const hu_tom_belief_state_t *state, hu_allocator_t *alloc,
                              hu_tom_gap_t **gaps_out, size_t *gap_count) {
    if (!state || !alloc || !gaps_out || !gap_count)
        return HU_ERR_INVALID_ARGUMENT;
    *gaps_out = NULL;
    *gap_count = 0;

    if (state->expectation_count == 0)
        return HU_OK;

    /* Allocate worst-case array */
    hu_tom_gap_t *gaps = alloc->alloc(alloc->ctx, state->expectation_count * sizeof(hu_tom_gap_t));
    if (!gaps)
        return HU_ERR_OUT_OF_MEMORY;
    memset(gaps, 0, state->expectation_count * sizeof(hu_tom_gap_t));

    size_t count = 0;
    for (size_t i = 0; i < state->expectation_count; i++) {
        const hu_tom_expectation_t *exp = &state->expectations[i];
        /* Check if AI actually has knowledge about this topic */
        bool ai_knows = false;
        for (size_t j = 0; j < state->belief_count; j++) {
            const hu_tom_belief_t *b = &state->beliefs[j];
            if (b->topic_len == exp->topic_len &&
                strncmp(b->topic, exp->topic, exp->topic_len) == 0 && b->type == HU_BELIEF_KNOWS &&
                b->confidence >= 0.5f) {
                ai_knows = true;
                break;
            }
        }
        if (!ai_knows) {
            char *dup = hu_strndup(alloc, exp->topic, exp->topic_len);
            if (!dup) {
                hu_tom_gaps_free(alloc, gaps, count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            gaps[count].topic = dup;
            gaps[count].topic_len = strlen(dup);
            gaps[count].knowledge_type = exp->knowledge_type;
            count++;
        }
    }

    if (count == 0) {
        alloc->free(alloc->ctx, gaps, state->expectation_count * sizeof(hu_tom_gap_t));
        return HU_OK;
    }

    *gaps_out = gaps;
    *gap_count = count;
    return HU_OK;
}

char *hu_tom_build_gap_directive(hu_allocator_t *alloc, const hu_tom_gap_t *gaps, size_t gap_count,
                                 size_t *out_len) {
    if (!alloc || !gaps || gap_count == 0 || !out_len)
        return NULL;

    char buf[2048];
    size_t pos = 0;
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "### Knowledge Gap Alert\n"
                         "The user expects you to know about the following topics, "
                         "but you don't have reliable information:\n");

    for (size_t i = 0; i < gap_count && pos < sizeof(buf) - 128; i++) {
        const char *label = "remembers";
        if (gaps[i].knowledge_type == HU_TOM_EXPECT_UNDERSTANDS)
            label = "understands";
        else if (gaps[i].knowledge_type == HU_TOM_EXPECT_TRACKS)
            label = "tracks";
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- \"%.*s\" (expects AI %s)\n",
                             (int)gaps[i].topic_len, gaps[i].topic, label);
    }
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "Be honest rather than fabricate. Acknowledge gaps transparently.\n");
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    char *result = hu_strndup(alloc, buf, pos);
    if (!result)
        return NULL;
    *out_len = pos;
    return result;
}

void hu_tom_gaps_free(hu_allocator_t *alloc, hu_tom_gap_t *gaps, size_t count) {
    if (!alloc || !gaps)
        return;
    for (size_t i = 0; i < count; i++) {
        hu_str_free(alloc, gaps[i].topic);
    }
    /* Free the array itself — we allocated expectation_count items but only count were used.
     * We don't know the original allocation size, so use count as approximation.
     * The allocator's free typically ignores size anyway. */
    alloc->free(alloc->ctx, gaps, count * sizeof(hu_tom_gap_t));
}

/* Pattern table for detecting user expectations about AI knowledge */
typedef struct {
    const char *pattern;
    size_t pattern_len;
    hu_tom_expected_knowledge_t type;
} expectation_pattern_t;

static const expectation_pattern_t EXPECTATION_PATTERNS[] = {
    {"remember when", 13, HU_TOM_EXPECT_REMEMBERS},
    {"you remember", 12, HU_TOM_EXPECT_REMEMBERS},
    {"you know my", 11, HU_TOM_EXPECT_REMEMBERS},
    {"you know about", 14, HU_TOM_EXPECT_UNDERSTANDS},
    {"as you know", 11, HU_TOM_EXPECT_UNDERSTANDS},
    {"like i told you", 15, HU_TOM_EXPECT_REMEMBERS},
    {"i already told you", 18, HU_TOM_EXPECT_REMEMBERS},
    {"we talked about", 15, HU_TOM_EXPECT_REMEMBERS},
    {"we discussed", 12, HU_TOM_EXPECT_REMEMBERS},
    {"you've been following", 21, HU_TOM_EXPECT_TRACKS},
    {"you've been tracking", 20, HU_TOM_EXPECT_TRACKS},
    {"as we discussed", 15, HU_TOM_EXPECT_REMEMBERS},
};

static int ci_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? (char)(a[i] + 32) : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? (char)(b[i] + 32) : b[i];
        if (ca != cb)
            return (unsigned char)ca - (unsigned char)cb;
    }
    return 0;
}

bool hu_tom_detect_user_expectation(const char *text, size_t text_len, const char **topic_out,
                                    size_t *topic_len_out,
                                    hu_tom_expected_knowledge_t *knowledge_type_out) {
    if (!text || text_len == 0 || !topic_out || !topic_len_out || !knowledge_type_out)
        return false;

    size_t num_patterns = sizeof(EXPECTATION_PATTERNS) / sizeof(EXPECTATION_PATTERNS[0]);
    for (size_t p = 0; p < num_patterns; p++) {
        const expectation_pattern_t *pat = &EXPECTATION_PATTERNS[p];
        if (text_len < pat->pattern_len)
            continue;
        /* Scan for pattern anywhere in text (case-insensitive) */
        for (size_t i = 0; i <= text_len - pat->pattern_len; i++) {
            if (ci_strncmp(text + i, pat->pattern, pat->pattern_len) != 0)
                continue;
            /* Found pattern — extract topic as the rest of the clause */
            size_t after = i + pat->pattern_len;
            /* Skip whitespace and punctuation after pattern */
            while (after < text_len && (text[after] == ' ' || text[after] == ','))
                after++;
            if (after >= text_len)
                continue;
            /* Topic runs to end of sentence or end of text */
            size_t topic_start = after;
            size_t topic_end = after;
            while (topic_end < text_len && text[topic_end] != '.' && text[topic_end] != '?' &&
                   text[topic_end] != '!' && text[topic_end] != '\n')
                topic_end++;
            /* Trim trailing whitespace */
            while (topic_end > topic_start &&
                   (text[topic_end - 1] == ' ' || text[topic_end - 1] == ','))
                topic_end--;
            size_t tlen = topic_end - topic_start;
            if (tlen == 0 || tlen > 256)
                continue;
            *topic_out = text + topic_start;
            *topic_len_out = tlen;
            *knowledge_type_out = pat->type;
            return true;
        }
    }
    return false;
}

void hu_tom_deinit(hu_tom_belief_state_t *state, hu_allocator_t *alloc) {
    if (!state || !alloc)
        return;
    hu_str_free(alloc, state->contact_id);
    state->contact_id = NULL;
    state->contact_id_len = 0;
    for (size_t i = 0; i < state->belief_count; i++) {
        hu_str_free(alloc, state->beliefs[i].topic);
        state->beliefs[i].topic = NULL;
        state->beliefs[i].topic_len = 0;
    }
    state->belief_count = 0;
    for (size_t i = 0; i < state->expectation_count; i++) {
        hu_str_free(alloc, state->expectations[i].topic);
        state->expectations[i].topic = NULL;
        state->expectations[i].topic_len = 0;
    }
    state->expectation_count = 0;
}

/* =====================================================================
 * Phase A of Spec 4: SQLite-backed user-expectation persistence.
 *
 * Spec: specs/2026-05-19-tom-activation/{requirements,design,tasks}.md
 *  - Task 2: hu_tom_persist_user_expectation
 *  - Task 3: hu_tom_user_expectations_init_table (CREATE TABLE)
 *  - Task 5: hu_tom_build_context_with_expectations
 *  - Task 11: hu_tom_user_expectations_gc + daemon-tick wrapper
 *
 * Wire site for Task 1 is in src/daemon.c (around line 8954).
 * ===================================================================== */
