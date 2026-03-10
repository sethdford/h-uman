#include "human/security/replay.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct hu_replay_recorder {
    hu_allocator_t *alloc;
    hu_replay_event_t *events;
    size_t count;
    size_t capacity;
};

hu_replay_recorder_t *hu_replay_recorder_create(hu_allocator_t *alloc, uint32_t max_events) {
    if (!alloc || max_events == 0)
        return NULL;
    hu_replay_recorder_t *r = (hu_replay_recorder_t *)alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->capacity = max_events;
    r->events =
        (hu_replay_event_t *)alloc->alloc(alloc->ctx, max_events * sizeof(hu_replay_event_t));
    if (!r->events) {
        alloc->free(alloc->ctx, r, sizeof(*r));
        return NULL;
    }
    memset(r->events, 0, max_events * sizeof(hu_replay_event_t));
    return r;
}

void hu_replay_recorder_destroy(hu_replay_recorder_t *rec) {
    if (!rec)
        return;
    for (size_t i = 0; i < rec->count; i++) {
        if (rec->events[i].data)
            rec->alloc->free(rec->alloc->ctx, rec->events[i].data, rec->events[i].data_len + 1);
    }
    rec->alloc->free(rec->alloc->ctx, rec->events, rec->capacity * sizeof(hu_replay_event_t));
    rec->alloc->free(rec->alloc->ctx, rec, sizeof(*rec));
}

hu_error_t hu_replay_record(hu_replay_recorder_t *rec, hu_replay_event_type_t type,
                            const char *data, size_t data_len) {
    if (!rec)
        return HU_ERR_INVALID_ARGUMENT;
    if (rec->count >= rec->capacity)
        return HU_ERR_OUT_OF_MEMORY;

    hu_replay_event_t *e = &rec->events[rec->count];
    e->type = type;
    e->timestamp = (int64_t)time(NULL);
    e->data = data ? hu_strndup(rec->alloc, data, data_len) : NULL;
    e->data_len = data_len;
    rec->count++;
    return HU_OK;
}

static const char *event_type_str(hu_replay_event_type_t t) {
    switch (t) {
    case HU_REPLAY_TOOL_CALL:
        return "tool_call";
    case HU_REPLAY_TOOL_RESULT:
        return "tool_result";
    case HU_REPLAY_PROVIDER_REQUEST:
        return "provider_request";
    case HU_REPLAY_PROVIDER_RESPONSE:
        return "provider_response";
    case HU_REPLAY_USER_MESSAGE:
        return "user_message";
    case HU_REPLAY_AGENT_RESPONSE:
        return "agent_response";
    default:
        return "unknown";
    }
}

hu_error_t hu_replay_export_json(hu_replay_recorder_t *rec, hu_allocator_t *alloc, char **out_json,
                                 size_t *out_len) {
    if (!rec || !alloc || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 256 + rec->count * 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "{\"events\":[");
    for (size_t i = 0; i < rec->count && off < cap - 64; i++) {
        if (i > 0)
            buf[off++] = ',';
        hu_replay_event_t *e = &rec->events[i];
        off += (size_t)snprintf(buf + off, cap - off,
                                "{\"type\":\"%s\",\"timestamp\":%lld,\"data_len\":%zu}",
                                event_type_str(e->type), (long long)e->timestamp, e->data_len);
    }
    off += (size_t)snprintf(buf + off, cap - off, "]}");

    *out_json = buf;
    *out_len = off;
    return HU_OK;
}

size_t hu_replay_event_count(hu_replay_recorder_t *rec) {
    return rec ? rec->count : 0;
}

hu_error_t hu_replay_get_event(hu_replay_recorder_t *rec, size_t index, hu_replay_event_t *out) {
    if (!rec || !out || index >= rec->count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = rec->events[index];
    return HU_OK;
}
