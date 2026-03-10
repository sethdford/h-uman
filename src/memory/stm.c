#include "human/memory/stm.h"
#include "human/core/string.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *EMOTION_NAMES[] = {
    "neutral", "joy", "sadness", "anger", "fear",
    "surprise", "frustration", "excitement", "anxiety",
};

hu_error_t hu_stm_init(hu_stm_buffer_t *buf, hu_allocator_t alloc, const char *session_id,
                       size_t session_id_len) {
    if (!buf)
        return HU_ERR_INVALID_ARGUMENT;
    memset(buf, 0, sizeof(*buf));
    buf->alloc = alloc;
    if (session_id && session_id_len > 0) {
        buf->session_id = hu_strndup(&buf->alloc, session_id, session_id_len);
        if (!buf->session_id)
            return HU_ERR_OUT_OF_MEMORY;
        buf->session_id_len = session_id_len;
    }
    return HU_OK;
}

static void free_turn(hu_stm_turn_t *t, hu_allocator_t *alloc) {
    if (!t || !alloc)
        return;
    if (t->role) {
        alloc->free(alloc->ctx, t->role, strlen(t->role) + 1);
        t->role = NULL;
    }
    if (t->content) {
        alloc->free(alloc->ctx, t->content, t->content_len + 1);
        t->content = NULL;
    }
    t->content_len = 0;
    for (size_t i = 0; i < t->entity_count; i++) {
        if (t->entities[i].name)
            alloc->free(alloc->ctx, t->entities[i].name, t->entities[i].name_len + 1);
        if (t->entities[i].type)
            alloc->free(alloc->ctx, t->entities[i].type, t->entities[i].type_len + 1);
    }
    t->entity_count = 0;
    if (t->primary_topic) {
        alloc->free(alloc->ctx, t->primary_topic, strlen(t->primary_topic) + 1);
        t->primary_topic = NULL;
    }
    t->occupied = false;
}

hu_error_t hu_stm_record_turn(hu_stm_buffer_t *buf, const char *role, size_t role_len,
                              const char *content, size_t content_len, uint64_t timestamp_ms) {
    if (!buf || !role || !content)
        return HU_ERR_INVALID_ARGUMENT;

    size_t slot;
    if (buf->turn_count >= HU_STM_MAX_TURNS) {
        slot = buf->head;
        free_turn(&buf->turns[slot], &buf->alloc);
        buf->head = (buf->head + 1) % HU_STM_MAX_TURNS;
    } else {
        slot = buf->turn_count;
    }

    hu_stm_turn_t *t = &buf->turns[slot];
    t->role = hu_strndup(&buf->alloc, role, role_len);
    if (!t->role)
        return HU_ERR_OUT_OF_MEMORY;
    t->content = hu_strndup(&buf->alloc, content, content_len);
    if (!t->content) {
        buf->alloc.free(buf->alloc.ctx, t->role, role_len + 1);
        t->role = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    t->content_len = content_len;
    t->timestamp_ms = timestamp_ms;
    t->occupied = true;
    t->entity_count = 0;
    t->emotion_count = 0;
    t->primary_topic = NULL;

    buf->turn_count++;
    return HU_OK;
}

static hu_stm_turn_t *get_turn_mutable(hu_stm_buffer_t *buf, size_t idx) {
    if (!buf)
        return NULL;
    size_t n = hu_stm_count(buf);
    if (idx >= n)
        return NULL;
    size_t physical;
    if (buf->turn_count <= HU_STM_MAX_TURNS) {
        physical = idx;
    } else {
        physical = (buf->head + idx) % HU_STM_MAX_TURNS;
    }
    hu_stm_turn_t *t = &buf->turns[physical];
    return t->occupied ? t : NULL;
}

hu_error_t hu_stm_turn_add_entity(hu_stm_buffer_t *buf, size_t turn_idx, const char *name,
                                  size_t name_len, const char *type, size_t type_len,
                                  uint32_t mention_count) {
    hu_stm_turn_t *t = get_turn_mutable(buf, turn_idx);
    if (!t || !name || name_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (t->entity_count >= HU_STM_MAX_ENTITIES)
        return HU_ERR_OUT_OF_MEMORY;

    hu_stm_entity_t *e = &t->entities[t->entity_count];
    e->name = hu_strndup(&buf->alloc, name, name_len);
    if (!e->name)
        return HU_ERR_OUT_OF_MEMORY;
    e->name_len = name_len;
    e->type = (type && type_len > 0) ? hu_strndup(&buf->alloc, type, type_len)
                                     : hu_strndup(&buf->alloc, "entity", 6);
    if (!e->type) {
        buf->alloc.free(buf->alloc.ctx, e->name, name_len + 1);
        e->name = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    e->type_len = type && type_len > 0 ? type_len : 6;
    e->mention_count = mention_count;
    e->importance = 0.0;
    t->entity_count++;
    return HU_OK;
}

hu_error_t hu_stm_turn_add_emotion(hu_stm_buffer_t *buf, size_t turn_idx, hu_emotion_tag_t tag,
                                   double intensity) {
    hu_stm_turn_t *t = get_turn_mutable(buf, turn_idx);
    if (!t)
        return HU_ERR_INVALID_ARGUMENT;
    if (t->emotion_count >= HU_STM_MAX_EMOTIONS)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < t->emotion_count; i++) {
        if (t->emotions[i].tag == tag)
            return HU_OK;
    }
    t->emotions[t->emotion_count].tag = tag;
    t->emotions[t->emotion_count].intensity = intensity;
    t->emotion_count++;
    return HU_OK;
}

hu_error_t hu_stm_turn_set_primary_topic(hu_stm_buffer_t *buf, size_t turn_idx, const char *topic,
                                         size_t topic_len) {
    hu_stm_turn_t *t = get_turn_mutable(buf, turn_idx);
    if (!t)
        return HU_ERR_INVALID_ARGUMENT;
    if (t->primary_topic) {
        buf->alloc.free(buf->alloc.ctx, t->primary_topic, strlen(t->primary_topic) + 1);
        t->primary_topic = NULL;
    }
    if (!topic || topic_len == 0)
        return HU_OK;
    t->primary_topic = hu_strndup(&buf->alloc, topic, topic_len);
    if (!t->primary_topic)
        return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}

size_t hu_stm_count(const hu_stm_buffer_t *buf) {
    if (!buf)
        return 0;
    return buf->turn_count > HU_STM_MAX_TURNS ? HU_STM_MAX_TURNS : buf->turn_count;
}

const hu_stm_turn_t *hu_stm_get(const hu_stm_buffer_t *buf, size_t idx) {
    if (!buf)
        return NULL;
    size_t n = hu_stm_count(buf);
    if (idx >= n)
        return NULL;
    size_t physical;
    if (buf->turn_count <= HU_STM_MAX_TURNS) {
        physical = idx;
    } else {
        physical = (buf->head + idx) % HU_STM_MAX_TURNS;
    }
    const hu_stm_turn_t *t = &buf->turns[physical];
    return t->occupied ? t : NULL;
}

hu_error_t hu_stm_build_context(const hu_stm_buffer_t *buf, hu_allocator_t *alloc, char **out,
                                size_t *out_len) {
    if (!buf || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 4096;
    char *result = (char *)alloc->alloc(alloc->ctx, cap);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;

    static const char header[] = "## Session Context\n\n";
    size_t len = sizeof(header) - 1;
    if (len >= cap) {
        alloc->free(alloc->ctx, result, cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(result, header, len);

    size_t n = hu_stm_count(buf);
    for (size_t i = 0; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t || !t->role || !t->content)
            continue;

        const char *role = t->role;
        size_t role_len = strlen(role);
        size_t content_len = t->content_len;
        size_t need =
            6 + role_len + 2 + content_len + 2; /* "**" + role + "**: " + content + "\n\n" */

        while (len + need + 1 > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, result, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, result, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            result = nb;
            cap = new_cap;
        }

        result[len++] = '*';
        result[len++] = '*';
        memcpy(result + len, role, role_len);
        len += role_len;
        result[len++] = '*';
        result[len++] = '*';
        result[len++] = ':';
        result[len++] = ' ';
        memcpy(result + len, t->content, content_len);
        len += content_len;
        result[len++] = '\n';
        result[len++] = '\n';
    }

    size_t user_start = 0;
    size_t user_count = 0;
    for (size_t i = n; i > 0; i--) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i - 1);
        if (t && t->role && strcmp(t->role, "user") == 0) {
            user_start = i - 1;
            user_count++;
            if (user_count >= 5)
                break;
        }
    }

    double max_intensity[9];
    for (size_t i = 0; i < 9; i++)
        max_intensity[i] = 0.0;
    double pos_sum = 0.0, neg_sum = 0.0;

    for (size_t i = user_start; i < n; i++) {
        const hu_stm_turn_t *t = hu_stm_get(buf, i);
        if (!t || !t->role || strcmp(t->role, "user") != 0)
            continue;
        for (size_t j = 0; j < t->emotion_count; j++) {
            double intensity = t->emotions[j].intensity;
            if (intensity < 0.3)
                continue;
            hu_emotion_tag_t tag = t->emotions[j].tag;
            size_t tag_idx = (size_t)tag;
            if (tag_idx >= sizeof(EMOTION_NAMES) / sizeof(EMOTION_NAMES[0]))
                continue;
            if (intensity > max_intensity[tag_idx])
                max_intensity[tag_idx] = intensity;
            if (tag == HU_EMOTION_JOY || tag == HU_EMOTION_EXCITEMENT || tag == HU_EMOTION_SURPRISE)
                pos_sum += intensity;
            else if (tag != HU_EMOTION_NEUTRAL)
                neg_sum += intensity;
        }
    }

    char emotion_buf[256];
    size_t emotion_buf_len = 0;
    size_t emotion_entries = 0;
    for (size_t tag_idx = 0; tag_idx < 9; tag_idx++) {
        if (max_intensity[tag_idx] < 0.3)
            continue;
        const char *label = max_intensity[tag_idx] >= 0.7 ? "high"
            : (max_intensity[tag_idx] >= 0.4 ? "moderate" : "low");
        int written = snprintf(emotion_buf + emotion_buf_len,
                              sizeof(emotion_buf) - emotion_buf_len,
                              "%s%s (%s)",
                              emotion_buf_len > 0 ? ", " : "",
                              EMOTION_NAMES[tag_idx],
                              label);
        if (written > 0 && (size_t)written < sizeof(emotion_buf) - emotion_buf_len) {
            emotion_buf_len += (size_t)written;
            emotion_entries++;
        }
    }

    if (emotion_entries > 0) {
        const char *trend = pos_sum > neg_sum ? "mostly positive" : (neg_sum > pos_sum ? "mostly negative" : "mixed");
        char traj[512];
        int traj_len = snprintf(traj, sizeof(traj),
                                "\n### Emotional Trajectory\nRecent user emotions: %s\nTrend: %s\n",
                                emotion_buf, trend);
        if (traj_len > 0 && (size_t)traj_len < sizeof(traj)) {
            size_t need = (size_t)traj_len;
            while (len + need + 1 > cap) {
                size_t new_cap = cap * 2;
                char *nb = (char *)alloc->realloc(alloc->ctx, result, cap, new_cap);
                if (!nb) {
                    alloc->free(alloc->ctx, result, cap);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                result = nb;
                cap = new_cap;
            }
            memcpy(result + len, traj, (size_t)traj_len);
            len += (size_t)traj_len;
        }
    }

    result[len] = '\0';
    *out = result;
    *out_len = len;
    return HU_OK;
}

void hu_stm_clear(hu_stm_buffer_t *buf) {
    if (!buf)
        return;
    for (size_t i = 0; i < HU_STM_MAX_TURNS; i++) {
        if (buf->turns[i].occupied)
            free_turn(&buf->turns[i], &buf->alloc);
    }
    for (size_t i = 0; i < buf->topic_count; i++) {
        if (buf->topics[i]) {
            buf->alloc.free(buf->alloc.ctx, buf->topics[i], strlen(buf->topics[i]) + 1);
            buf->topics[i] = NULL;
        }
    }
    buf->turn_count = 0;
    buf->head = 0;
    buf->topic_count = 0;
}

void hu_stm_deinit(hu_stm_buffer_t *buf) {
    if (!buf)
        return;
    hu_stm_clear(buf);
    if (buf->session_id) {
        buf->alloc.free(buf->alloc.ctx, buf->session_id, buf->session_id_len + 1);
        buf->session_id = NULL;
        buf->session_id_len = 0;
    }
}
