#include "seaclaw/context/mood.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stdio.h>
#include <string.h>

#define SC_MOOD_MAX_TAGS 16
#define SC_MOOD_TAG_LEN  32

/* Emotion tags from stm.h / promotion.c */
static const char *const SC_MOOD_POSITIVE[] = {"joy", "excitement", "surprise"};
static const size_t SC_MOOD_POSITIVE_COUNT =
    sizeof(SC_MOOD_POSITIVE) / sizeof(SC_MOOD_POSITIVE[0]);
static const char *const SC_MOOD_NEGATIVE[] = {"sadness", "anger", "fear", "frustration", "anxiety"};
static const size_t SC_MOOD_NEGATIVE_COUNT =
    sizeof(SC_MOOD_NEGATIVE) / sizeof(SC_MOOD_NEGATIVE[0]);

typedef struct mood_tag_agg {
    char tag[SC_MOOD_TAG_LEN];
    double sum_intensity;
    size_t count;
} mood_tag_agg_t;

static bool key_matches_contact(const char *key, size_t key_len, const char *contact_id,
                                size_t contact_id_len) {
    if (!key || key_len < 10 || !contact_id || contact_id_len == 0)
        return true; /* no filter */
    size_t prefix_len = 9 + contact_id_len + 1; /* "emotion:" + contact_id + ":" */
    if (key_len < prefix_len)
        return false;
    if (memcmp(key, "emotion:", 8) != 0)
        return false;
    if (memcmp(key + 8, contact_id, contact_id_len) != 0)
        return false;
    if (key[8 + contact_id_len] != ':')
        return false;
    return true;
}

static bool parse_emotion_json(const char *content, size_t content_len, char *tag_out,
                                size_t tag_cap, double *intensity_out) {
    if (!content || content_len < 10 || !tag_out || tag_cap < 2 || !intensity_out)
        return false;
    *intensity_out = 0.0;

    const char *tag_start = strstr(content, "\"tag\"");
    if (!tag_start || tag_start >= content + content_len)
        return false;
    const char *colon = strchr(tag_start, ':');
    if (!colon || colon >= content + content_len)
        return false;
    const char *quote = strchr(colon + 1, '"');
    if (!quote || quote >= content + content_len)
        return false;
    const char *tag_end = strchr(quote + 1, '"');
    if (!tag_end || tag_end - quote - 1 <= 0 || (size_t)(tag_end - quote - 1) >= tag_cap)
        return false;
    size_t tag_len = (size_t)(tag_end - quote - 1);
    memcpy(tag_out, quote + 1, tag_len);
    tag_out[tag_len] = '\0';

    const char *int_start = strstr(content, "\"intensity\"");
    if (!int_start || int_start >= content + content_len)
        return false;
    const char *int_colon = strchr(int_start, ':');
    if (!int_colon || int_colon >= content + content_len)
        return false;
    int n = 0;
    if (sscanf(int_colon + 1, "%lf%n", intensity_out, &n) < 1)
        return false;
    if (*intensity_out < 0.0)
        *intensity_out = 0.0;
    if (*intensity_out > 1.0)
        *intensity_out = 1.0;
    return true;
}

static bool is_positive(const char *tag) {
    for (size_t i = 0; i < SC_MOOD_POSITIVE_COUNT; i++) {
        if (strcmp(tag, SC_MOOD_POSITIVE[i]) == 0)
            return true;
    }
    return false;
}

static bool is_negative(const char *tag) {
    for (size_t i = 0; i < SC_MOOD_NEGATIVE_COUNT; i++) {
        if (strcmp(tag, SC_MOOD_NEGATIVE[i]) == 0)
            return true;
    }
    return false;
}

static const char *intensity_label(double avg) {
    if (avg >= 0.7)
        return "high";
    if (avg >= 0.4)
        return "moderate";
    return "low";
}

sc_error_t sc_mood_build_context(sc_allocator_t *alloc, sc_memory_t *memory,
                                 const char *contact_id, size_t contact_id_len,
                                 char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory || !memory->vtable || !memory->vtable->list)
        return SC_ERR_INVALID_ARGUMENT;

    static const char emotions_cat[] = "emotions";
    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = emotions_cat, .name_len = sizeof(emotions_cat) - 1},
    };

    /* List all emotions; we filter by key prefix "emotion:<contact_id>:" since
     * session_id may vary by backend (e.g. buf->session_id vs contact_id). */
    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->list(memory->ctx, alloc, &cat, NULL, 0, &entries, &count);
    if (err != SC_OK)
        return err;
    if (!entries || count == 0) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
        return SC_OK;
    }

    mood_tag_agg_t aggs[SC_MOOD_MAX_TAGS];
    size_t agg_count = 0;
    memset(aggs, 0, sizeof(aggs));

    for (size_t i = 0; i < count; i++) {
        const sc_memory_entry_t *e = &entries[i];
        if (!e->content || e->content_len == 0)
            continue;
        if (contact_id && contact_id_len > 0 && e->key && e->key_len > 0 &&
            !key_matches_contact(e->key, e->key_len, contact_id, contact_id_len))
            continue;

        char tag_buf[SC_MOOD_TAG_LEN];
        double intensity = 0.0;
        if (!parse_emotion_json(e->content, e->content_len, tag_buf, sizeof(tag_buf), &intensity))
            continue;

        size_t j = 0;
        for (; j < agg_count; j++) {
            if (strcmp(aggs[j].tag, tag_buf) == 0) {
                aggs[j].sum_intensity += intensity;
                aggs[j].count++;
                break;
            }
        }
        if (j >= agg_count && agg_count < SC_MOOD_MAX_TAGS) {
            size_t tag_len = strlen(tag_buf);
            if (tag_len >= SC_MOOD_TAG_LEN)
                tag_len = SC_MOOD_TAG_LEN - 1;
            memcpy(aggs[agg_count].tag, tag_buf, tag_len);
            aggs[agg_count].tag[tag_len] = '\0';
            aggs[agg_count].sum_intensity = intensity;
            aggs[agg_count].count = 1;
            agg_count++;
        }
    }

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));

    if (agg_count == 0)
        return SC_OK;

    /* Find dominant (highest total intensity) */
    size_t dominant_idx = 0;
    double dominant_total = aggs[0].sum_intensity;
    for (size_t i = 1; i < agg_count; i++) {
        if (aggs[i].sum_intensity > dominant_total) {
            dominant_total = aggs[i].sum_intensity;
            dominant_idx = i;
        }
    }

    /* Compute trend: positive vs negative total intensity */
    double pos_total = 0.0;
    double neg_total = 0.0;
    for (size_t i = 0; i < agg_count; i++) {
        double total = aggs[i].sum_intensity;
        if (is_positive(aggs[i].tag))
            pos_total += total;
        else if (is_negative(aggs[i].tag))
            neg_total += total;
    }
    const char *trend = "mixed";
    if (pos_total > neg_total * 1.2)
        trend = "mostly positive";
    else if (neg_total > pos_total * 1.2)
        trend = "mostly negative";

    char buf[1024];
    size_t pos = 0;
    int w = snprintf(buf, sizeof(buf),
                     "### Mood Overview\n"
                     "Recent emotional patterns for this contact:\n"
                     "- Dominant mood: %s (avg intensity: %s)\n",
                     aggs[dominant_idx].tag,
                     intensity_label(aggs[dominant_idx].count > 0
                                         ? aggs[dominant_idx].sum_intensity / aggs[dominant_idx].count
                                         : 0.0));
    if (w > 0 && (size_t)w < sizeof(buf))
        pos = (size_t)w;

    /* Also present: other tags (excluding dominant) */
    bool first_also = true;
    for (size_t i = 0; i < agg_count && pos < sizeof(buf) - 64; i++) {
        if (i == dominant_idx)
            continue;
        double avg = aggs[i].count > 0 ? aggs[i].sum_intensity / aggs[i].count : 0.0;
        if (first_also) {
            w = snprintf(buf + pos, sizeof(buf) - pos, "- Also present: %s (%s)", aggs[i].tag,
                         intensity_label(avg));
            first_also = false;
        } else {
            w = snprintf(buf + pos, sizeof(buf) - pos, ", %s (%s)", aggs[i].tag,
                         intensity_label(avg));
        }
        if (w > 0 && pos + (size_t)w < sizeof(buf))
            pos += (size_t)w;
    }
    if (!first_also) {
        w = snprintf(buf + pos, sizeof(buf) - pos, "\n");
        if (w > 0 && pos + (size_t)w < sizeof(buf))
            pos += (size_t)w;
    }

    w = snprintf(buf + pos, sizeof(buf) - pos, "- Overall trend: %s\n", trend);
    if (w > 0 && pos + (size_t)w < sizeof(buf))
        pos += (size_t)w;

    char *result = (char *)alloc->alloc(alloc->ctx, pos + 1);
    if (!result)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out = result;
    *out_len = pos;
    return SC_OK;
}
