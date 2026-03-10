#include "human/agent/theory_of_mind.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

hu_error_t hu_tom_init(hu_belief_state_t *state, hu_allocator_t *alloc, const char *contact_id,
                       size_t contact_id_len) {
    if (!state || !alloc || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    size_t len = contact_id_len;
    if (len == 0)
        len = strlen(contact_id);
    char *dup = hu_strndup(alloc, contact_id, len);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    state->contact_id = dup;
    state->contact_id_len = strlen(dup);
    state->belief_count = 0;
    memset(state->beliefs, 0, sizeof(state->beliefs));
    return HU_OK;
}

static hu_belief_t *find_belief_by_topic(hu_belief_state_t *state, const char *topic,
                                         size_t topic_len) {
    for (size_t i = 0; i < state->belief_count; i++) {
        hu_belief_t *b = &state->beliefs[i];
        if (!b->topic || b->topic_len != topic_len)
            continue;
        if (strncmp(b->topic, topic, topic_len) == 0)
            return b;
    }
    return NULL;
}

hu_error_t hu_tom_record_belief(hu_belief_state_t *state, hu_allocator_t *alloc, const char *topic,
                                size_t topic_len, hu_belief_type_t type, float confidence) {
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

    hu_belief_t *existing = find_belief_by_topic(state, topic, len);
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

    hu_belief_t *b = &state->beliefs[state->belief_count];
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

hu_error_t hu_tom_build_context(const hu_belief_state_t *state, hu_allocator_t *alloc, char **out,
                                size_t *out_len) {
    if (!state || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[4096];
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "### Contact Mental Model\n");

    for (int t = HU_BELIEF_KNOWS; t <= HU_BELIEF_MISTAKEN; t++) {
        hu_belief_type_t type = (hu_belief_type_t)t;
        bool first = true;
        for (size_t i = 0; i < state->belief_count; i++) {
            if (state->beliefs[i].type != type)
                continue;
            if (first) {
                pos +=
                    (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s: ", belief_type_label(type));
                first = false;
            } else {
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
            }
            if (pos >= sizeof(buf))
                break;
            if (belief_shows_confidence(type)) {
                int pct = (int)(state->beliefs[i].confidence * 100.0f + 0.5f);
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s (%d%%)",
                                        state->beliefs[i].topic, pct);
            } else {
                pos +=
                    (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s", state->beliefs[i].topic);
            }
            if (pos >= sizeof(buf))
                break;
        }
        if (!first)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "\n");
        if (pos >= sizeof(buf))
            break;
    }

    char *result = hu_strndup(alloc, buf, pos);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out = result;
    *out_len = pos;
    return HU_OK;
}

void hu_tom_deinit(hu_belief_state_t *state, hu_allocator_t *alloc) {
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
}
