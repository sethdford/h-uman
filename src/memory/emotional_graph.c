#include "human/memory/emotional_graph.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *EMOTION_NAMES[] = {
    "neutral", "joy", "sadness", "anger", "fear",
    "surprise", "frustration", "excitement", "anxiety",
};
#define HU_EGRAPH_EMOTION_COUNT (sizeof(EMOTION_NAMES) / sizeof(EMOTION_NAMES[0]))

static int topic_cmp_ci(const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t n = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)tolower((unsigned char)a[i]);
        int cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb)
            return ca - cb;
    }
    if (a_len != b_len)
        return (int)(a_len - b_len);
    return 0;
}

static size_t find_node(const hu_emotional_graph_t *g, const char *topic, size_t topic_len) {
    for (size_t i = 0; i < g->node_count; i++) {
        if (g->nodes[i].topic && g->nodes[i].topic_len == topic_len &&
            topic_cmp_ci(g->nodes[i].topic, g->nodes[i].topic_len, topic, topic_len) == 0)
            return i;
    }
    return (size_t)-1;
}

static size_t find_edge(const hu_emotional_graph_t *g, size_t topic_idx, hu_emotion_tag_t emotion) {
    for (size_t i = 0; i < g->edge_count; i++) {
        if (g->edges[i].topic_idx == topic_idx && g->edges[i].emotion == emotion)
            return i;
    }
    return (size_t)-1;
}

hu_error_t hu_egraph_init(hu_emotional_graph_t *g, hu_allocator_t alloc) {
    if (!g)
        return HU_ERR_INVALID_ARGUMENT;
    memset(g, 0, sizeof(*g));
    g->alloc = alloc;
    return HU_OK;
}

hu_error_t hu_egraph_record(hu_emotional_graph_t *g, const char *topic, size_t topic_len,
                            hu_emotion_tag_t emotion, double intensity) {
    if (!g || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    size_t topic_idx = find_node(g, topic, topic_len);
    if (topic_idx == (size_t)-1) {
        if (g->node_count >= HU_EGRAPH_MAX_NODES)
            return HU_ERR_OUT_OF_MEMORY;
        char *dup = hu_strndup(&g->alloc, topic, topic_len);
        if (!dup)
            return HU_ERR_OUT_OF_MEMORY;
        g->nodes[g->node_count].topic = dup;
        g->nodes[g->node_count].topic_len = topic_len;
        topic_idx = g->node_count++;
    }

    size_t edge_idx = find_edge(g, topic_idx, emotion);
    if (edge_idx != (size_t)-1) {
        hu_egraph_edge_t *e = &g->edges[edge_idx];
        uint32_t count = e->occurrence_count + 1;
        double old_avg = e->intensity;
        e->intensity = (old_avg * (double)e->occurrence_count + intensity) / (double)count;
        e->occurrence_count = count;
        return HU_OK;
    }

    if (g->edge_count >= HU_EGRAPH_MAX_EDGES)
        return HU_ERR_OUT_OF_MEMORY;

    g->edges[g->edge_count].topic_idx = topic_idx;
    g->edges[g->edge_count].emotion = emotion;
    g->edges[g->edge_count].intensity = intensity;
    g->edges[g->edge_count].occurrence_count = 1;
    g->edge_count++;
    return HU_OK;
}

hu_emotion_tag_t hu_egraph_query(const hu_emotional_graph_t *g, const char *topic, size_t topic_len,
                                 double *avg_intensity) {
    if (!g || !topic) {
        if (avg_intensity)
            *avg_intensity = 0.0;
        return HU_EMOTION_NEUTRAL;
    }

    size_t topic_idx = find_node(g, topic, topic_len);
    if (topic_idx == (size_t)-1) {
        if (avg_intensity)
            *avg_intensity = 0.0;
        return HU_EMOTION_NEUTRAL;
    }

    hu_emotion_tag_t best = HU_EMOTION_NEUTRAL;
    double best_intensity = 0.0;

    for (size_t i = 0; i < g->edge_count; i++) {
        if (g->edges[i].topic_idx == topic_idx && g->edges[i].intensity > best_intensity) {
            best_intensity = g->edges[i].intensity;
            best = g->edges[i].emotion;
        }
    }

    if (avg_intensity)
        *avg_intensity = best_intensity;
    return best;
}

static const char *intensity_label(double v) {
    if (v >= 0.7)
        return "high";
    if (v >= 0.4)
        return "moderate";
    return "low";
}

char *hu_egraph_build_context(hu_allocator_t *alloc, const hu_emotional_graph_t *g, size_t *out_len) {
    if (!alloc || !g || !out_len)
        return NULL;
    *out_len = 0;
    if (g->edge_count == 0)
        return NULL;

    size_t cap = 512;
    char *result = (char *)alloc->alloc(alloc->ctx, cap);
    if (!result)
        return NULL;

    static const char header[] = "### Emotional Topic Map\n";
    size_t len = sizeof(header) - 1;
    memcpy(result, header, len);

    for (size_t ni = 0; ni < g->node_count; ni++) {
        double avg = 0.0;
        hu_emotion_tag_t dom = hu_egraph_query(g, g->nodes[ni].topic, g->nodes[ni].topic_len, &avg);
        if (dom == HU_EMOTION_NEUTRAL && avg < 0.01)
            continue;

        size_t tag_idx = (size_t)dom;
        const char *emotion_name =
            (tag_idx < HU_EGRAPH_EMOTION_COUNT) ? EMOTION_NAMES[tag_idx] : "neutral";

        int w = snprintf(result + len, cap - len, "- %.*s → %s (%s)\n",
                         (int)g->nodes[ni].topic_len, g->nodes[ni].topic, emotion_name,
                         intensity_label(avg));
        if (w <= 0 || (size_t)w >= cap - len) {
            size_t new_cap = cap * 2;
            while (len + 256 > new_cap)
                new_cap *= 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, result, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, result, cap);
                return NULL;
            }
            result = nb;
            cap = new_cap;
            w = snprintf(result + len, cap - len, "- %.*s → %s (%s)\n",
                         (int)g->nodes[ni].topic_len, g->nodes[ni].topic, emotion_name,
                         intensity_label(avg));
        }
        if (w > 0 && (size_t)w < cap - len)
            len += (size_t)w;
    }

    if (len <= sizeof(header) - 1) {
        alloc->free(alloc->ctx, result, cap);
        return NULL;
    }

    result[len] = '\0';
    *out_len = len;
    return result;
}

hu_error_t hu_egraph_populate_from_stm(hu_emotional_graph_t *g, const hu_stm_buffer_t *buf) {
    if (!g || !buf)
        return HU_ERR_INVALID_ARGUMENT;

    size_t n = hu_stm_count(buf);
    for (size_t i = 0; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t || t->emotion_count == 0)
            continue;

        if (t->primary_topic && t->primary_topic[0]) {
            size_t pt_len = strlen(t->primary_topic);
            for (size_t j = 0; j < t->emotion_count; j++) {
                (void)hu_egraph_record(g, t->primary_topic, pt_len, t->emotions[j].tag,
                                       t->emotions[j].intensity);
            }
        }

        for (size_t e = 0; e < t->entity_count; e++) {
            const hu_stm_entity_t *ent = &t->entities[e];
            if (!ent->name || ent->name_len == 0)
                continue;
            if (ent->type && ent->type_len >= 5 &&
                topic_cmp_ci(ent->type, ent->type_len, "topic", 5) == 0) {
                for (size_t j = 0; j < t->emotion_count; j++) {
                    (void)hu_egraph_record(g, ent->name, ent->name_len, t->emotions[j].tag,
                                           t->emotions[j].intensity);
                }
            }
        }
    }
    return HU_OK;
}

void hu_egraph_deinit(hu_emotional_graph_t *g) {
    if (!g)
        return;
    for (size_t i = 0; i < g->node_count; i++) {
        if (g->nodes[i].topic) {
            g->alloc.free(g->alloc.ctx, g->nodes[i].topic, g->nodes[i].topic_len + 1);
            g->nodes[i].topic = NULL;
        }
    }
    g->node_count = 0;
    g->edge_count = 0;
}
