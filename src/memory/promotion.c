#include "human/memory/promotion.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_PROMOTION_HIGH_EMOTION_THRESHOLD 0.7
#define HU_PROMOTION_EMOTION_INTENSITY_THRESHOLD 0.3
#define HU_PROMOTION_RECENCY_TURNS          3

static bool entity_name_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return false;
    }
    return true;
}

typedef struct promoted_entity {
    char *name;
    size_t name_len;
    char *type;
    size_t type_len;
    uint32_t mention_count;
    double importance;
    size_t last_turn_idx;
} promoted_entity_t;

static uint32_t max_mention_count_in_buffer(const hu_stm_buffer_t *buf) {
    uint32_t max_m = 0;
    size_t n = hu_stm_count(buf);
    for (size_t i = 0; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t)
            continue;
        for (size_t j = 0; j < t->entity_count; j++) {
            const hu_stm_entity_t *e = &t->entities[j];
            if (e->mention_count > max_m)
                max_m = e->mention_count;
        }
    }
    return max_m > 0 ? max_m : 1;
}

static bool turn_has_high_intensity_emotion(const hu_stm_turn_t *t) {
    if (!t)
        return false;
    for (size_t i = 0; i < t->emotion_count; i++) {
        if (t->emotions[i].intensity >= HU_PROMOTION_HIGH_EMOTION_THRESHOLD)
            return true;
    }
    return false;
}

static double recency_weight(size_t last_turn_idx, size_t turn_count) {
    if (turn_count == 0)
        return 0.0;
    size_t turns_from_end = turn_count - 1 - last_turn_idx;
    if (turns_from_end < HU_PROMOTION_RECENCY_TURNS)
        return 1.0;
    /* Decay: 1.0 at last 3 turns, then linear decay over ~20 turns to ~0.5 */
    double decay = (double)(turns_from_end - HU_PROMOTION_RECENCY_TURNS) / 20.0;
    if (decay > 1.0)
        decay = 1.0;
    return 1.0 - decay * 0.5;
}

double hu_promotion_entity_importance(const hu_stm_entity_t *entity, const hu_stm_buffer_t *buf) {
    if (!entity || !buf || !entity->name)
        return 0.0;

    size_t n = hu_stm_count(buf);
    uint32_t max_mentions = max_mention_count_in_buffer(buf);

    /* Find last turn index where this entity appears */
    size_t last_turn_idx = 0;
    bool found = false;
    bool has_emotion_boost = false;
    for (size_t i = 0; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t)
            continue;
        for (size_t j = 0; j < t->entity_count; j++) {
            const hu_stm_entity_t *e = &t->entities[j];
            if (e->name && entity_name_eq(e->name, e->name_len, entity->name, entity->name_len)) {
                last_turn_idx = i;
                found = true;
                if (turn_has_high_intensity_emotion(t))
                    has_emotion_boost = true;
            }
        }
    }

    double mention_frac = (double)entity->mention_count / (double)max_mentions;
    if (mention_frac > 1.0)
        mention_frac = 1.0;

    double recency = found ? recency_weight(last_turn_idx, n) : 0.0;
    double emotion = has_emotion_boost ? 0.5 : 0.0;

    return mention_frac * 0.4 + recency * 0.3 + emotion * 0.3;
}

static int compare_promoted_by_importance(const void *a, const void *b) {
    const promoted_entity_t *pa = (const promoted_entity_t *)a;
    const promoted_entity_t *pb = (const promoted_entity_t *)b;
    if (pb->importance > pa->importance)
        return 1;
    if (pb->importance < pa->importance)
        return -1;
    return 0;
}

hu_error_t hu_promotion_run(hu_allocator_t *alloc, const hu_stm_buffer_t *buf, hu_memory_t *memory,
                            const hu_promotion_config_t *config) {
    if (!alloc || !buf || !memory || !memory->vtable || !config)
        return HU_ERR_INVALID_ARGUMENT;
    if (!memory->vtable->store)
        return HU_ERR_NOT_SUPPORTED;

    promoted_entity_t *collected = NULL;
    size_t collected_cap = 0;
    size_t collected_count = 0;

    size_t n = hu_stm_count(buf);

    for (size_t i = 0; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t)
            continue;
        for (size_t j = 0; j < t->entity_count; j++) {
            const hu_stm_entity_t *e = &t->entities[j];
            if (!e->name || e->name_len == 0)
                continue;

            /* Deduplicate by name */
            bool exists = false;
            for (size_t k = 0; k < collected_count; k++) {
                if (entity_name_eq(collected[k].name, collected[k].name_len, e->name,
                                   e->name_len)) {
                    exists = true;
                    collected[k].mention_count += e->mention_count;
                    if (i > collected[k].last_turn_idx)
                        collected[k].last_turn_idx = i;
                    break;
                }
            }
            if (exists)
                continue;

            if (collected_count >= collected_cap) {
                size_t new_cap = collected_cap ? collected_cap * 2 : 32;
                promoted_entity_t *new_arr;
                if (collected_cap == 0) {
                    new_arr = (promoted_entity_t *)alloc->alloc(alloc->ctx,
                                                                new_cap * sizeof(promoted_entity_t));
                } else {
                    new_arr = (promoted_entity_t *)alloc->realloc(
                        alloc->ctx, collected, collected_cap * sizeof(promoted_entity_t),
                        new_cap * sizeof(promoted_entity_t));
                }
                if (!new_arr) {
                    for (size_t k = 0; k < collected_count; k++) {
                        alloc->free(alloc->ctx, collected[k].name, collected[k].name_len + 1);
                        if (collected[k].type)
                            alloc->free(alloc->ctx, collected[k].type, collected[k].type_len + 1);
                    }
                    if (collected && collected_cap > 0)
                        alloc->free(alloc->ctx, collected,
                                    collected_cap * sizeof(promoted_entity_t));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                collected = new_arr;
                collected_cap = new_cap;
            }

            promoted_entity_t *pe = &collected[collected_count];
            pe->name = hu_strndup(alloc, e->name, e->name_len);
            if (!pe->name) {
                for (size_t k = 0; k < collected_count; k++) {
                    alloc->free(alloc->ctx, collected[k].name, collected[k].name_len + 1);
                    if (collected[k].type)
                        alloc->free(alloc->ctx, collected[k].type, collected[k].type_len + 1);
                }
                alloc->free(alloc->ctx, collected, collected_cap * sizeof(promoted_entity_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            pe->name_len = e->name_len;
            pe->type = (e->type && e->type_len > 0) ? hu_strndup(alloc, e->type, e->type_len)
                                                    : hu_strndup(alloc, "entity", 6);
            if (!pe->type) {
                alloc->free(alloc->ctx, pe->name, pe->name_len + 1);
                for (size_t k = 0; k < collected_count; k++) {
                    alloc->free(alloc->ctx, collected[k].name, collected[k].name_len + 1);
                    if (collected[k].type)
                        alloc->free(alloc->ctx, collected[k].type, collected[k].type_len + 1);
                }
                alloc->free(alloc->ctx, collected, collected_cap * sizeof(promoted_entity_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            pe->type_len = strlen(pe->type);
            pe->mention_count = e->mention_count;
            pe->last_turn_idx = i;
            pe->importance = 0.0; /* computed in second pass */
            collected_count++;
        }
    }

    if (collected_count == 0) {
        return HU_OK;
    }

    /* Second pass: compute importance for each aggregated entity */
    for (size_t k = 0; k < collected_count; k++) {
        promoted_entity_t *pe = &collected[k];
        hu_stm_entity_t synth = {
            .name = pe->name,
            .name_len = pe->name_len,
            .type = pe->type,
            .type_len = pe->type_len,
            .mention_count = pe->mention_count,
            .importance = 0.0,
        };
        pe->importance = hu_promotion_entity_importance(&synth, buf);
    }

    qsort(collected, collected_count, sizeof(promoted_entity_t), compare_promoted_by_importance);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE, .data = {{0}}};
    const char *session_id = buf->session_id;
    size_t session_id_len = buf->session_id_len;

    uint32_t promoted = 0;
    for (size_t k = 0; k < collected_count && promoted < config->max_entities; k++) {
        promoted_entity_t *pe = &collected[k];
        if (pe->mention_count < config->min_mention_count)
            continue;
        if (pe->importance < config->min_importance)
            continue;

        char key_buf[256];
        int kn = snprintf(key_buf, sizeof(key_buf), "entity:%.*s", (int)pe->name_len, pe->name);
        if (kn <= 0 || (size_t)kn >= sizeof(key_buf))
            continue;

        char content_buf[512];
        int cn = snprintf(content_buf, sizeof(content_buf),
                          "{\"type\":\"%.*s\",\"mentions\":%u,\"importance\":%.2f}",
                          (int)pe->type_len, pe->type, pe->mention_count, pe->importance);
        if (cn <= 0 || (size_t)cn >= sizeof(content_buf))
            continue;

        hu_error_t err = memory->vtable->store(memory->ctx, key_buf, (size_t)kn, content_buf,
                                               (size_t)cn, &cat, session_id, session_id_len);
        if (err == HU_OK)
            promoted++;
    }

    for (size_t k = 0; k < collected_count; k++) {
        alloc->free(alloc->ctx, collected[k].name, collected[k].name_len + 1);
        if (collected[k].type)
            alloc->free(alloc->ctx, collected[k].type, collected[k].type_len + 1);
    }
    alloc->free(alloc->ctx, collected, collected_cap * sizeof(promoted_entity_t));

    return HU_OK;
}

static const char *EMOTION_NAMES[] = {
    "neutral", "joy", "sadness", "anger", "fear",
    "surprise", "frustration", "excitement", "anxiety",
};
#define HU_EMOTION_NAME_COUNT (sizeof(EMOTION_NAMES) / sizeof(EMOTION_NAMES[0]))

hu_error_t hu_promotion_run_emotions(hu_allocator_t *alloc, const hu_stm_buffer_t *buf,
                                      hu_memory_t *memory, const char *contact_id,
                                      size_t contact_id_len) {
    if (!alloc || !buf || !memory || !memory->vtable)
        return HU_ERR_INVALID_ARGUMENT;
    if (!memory->vtable->store)
        return HU_ERR_NOT_SUPPORTED;

    static const char emotions_cat[] = "emotions";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = emotions_cat, .name_len = sizeof(emotions_cat) - 1},
    };

    const char *session_id = buf->session_id ? buf->session_id : "";
    size_t session_id_len = buf->session_id ? buf->session_id_len : 0;

    const char *cid = contact_id ? contact_id : "";
    size_t cid_len = contact_id ? contact_id_len : 0;

    size_t n = hu_stm_count(buf);
    for (size_t i = 0; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t)
            continue;
        for (size_t j = 0; j < t->emotion_count; j++) {
            const hu_stm_emotion_t *e = &t->emotions[j];
            if (e->intensity < HU_PROMOTION_EMOTION_INTENSITY_THRESHOLD)
                continue;
            if ((size_t)e->tag >= HU_EMOTION_NAME_COUNT)
                continue;

            const char *tag_name = EMOTION_NAMES[(size_t)e->tag];
            size_t tag_name_len = strlen(tag_name);

            char key_buf[384];
            int kn = snprintf(key_buf, sizeof(key_buf), "emotion:%.*s:%" PRIu64 ":%.*s",
                              (int)cid_len, cid, t->timestamp_ms, (int)tag_name_len, tag_name);
            if (kn <= 0 || (size_t)kn >= sizeof(key_buf))
                continue;

            char content_buf[128];
            int cn = snprintf(content_buf, sizeof(content_buf),
                              "{\"tag\":\"%.*s\",\"intensity\":%.2f,\"timestamp_ms\":%" PRIu64 "}",
                              (int)tag_name_len, tag_name, e->intensity, t->timestamp_ms);
            if (cn <= 0 || (size_t)cn >= sizeof(content_buf))
                continue;

            hu_error_t err = memory->vtable->store(memory->ctx, key_buf, (size_t)kn, content_buf,
                                                   (size_t)cn, &cat, session_id, session_id_len);
            (void)err;
        }
    }

    return HU_OK;
}
