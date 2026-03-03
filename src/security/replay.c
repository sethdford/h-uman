#include "seaclaw/security/replay.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct sc_replay_recorder {
    sc_allocator_t *alloc;
    sc_replay_event_t *events;
    size_t count;
    size_t capacity;
};

sc_replay_recorder_t *sc_replay_recorder_create(sc_allocator_t *alloc, uint32_t max_events) {
    if (!alloc || max_events == 0)
        return NULL;
    sc_replay_recorder_t *r = (sc_replay_recorder_t *)alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return NULL;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->capacity = max_events;
    r->events =
        (sc_replay_event_t *)alloc->alloc(alloc->ctx, max_events * sizeof(sc_replay_event_t));
    if (!r->events) {
        alloc->free(alloc->ctx, r, sizeof(*r));
        return NULL;
    }
    memset(r->events, 0, max_events * sizeof(sc_replay_event_t));
    return r;
}

void sc_replay_recorder_destroy(sc_replay_recorder_t *rec) {
    if (!rec)
        return;
    for (size_t i = 0; i < rec->count; i++) {
        if (rec->events[i].data)
            rec->alloc->free(rec->alloc->ctx, rec->events[i].data, rec->events[i].data_len + 1);
    }
    rec->alloc->free(rec->alloc->ctx, rec->events, rec->capacity * sizeof(sc_replay_event_t));
    rec->alloc->free(rec->alloc->ctx, rec, sizeof(*rec));
}

sc_error_t sc_replay_record(sc_replay_recorder_t *rec, sc_replay_event_type_t type,
                            const char *data, size_t data_len) {
    if (!rec)
        return SC_ERR_INVALID_ARGUMENT;
    if (rec->count >= rec->capacity)
        return SC_ERR_OUT_OF_MEMORY;

    sc_replay_event_t *e = &rec->events[rec->count];
    e->type = type;
    e->timestamp = (int64_t)time(NULL);
    e->data = data ? sc_strndup(rec->alloc, data, data_len) : NULL;
    e->data_len = data_len;
    rec->count++;
    return SC_OK;
}

static const char *event_type_str(sc_replay_event_type_t t) {
    switch (t) {
    case SC_REPLAY_TOOL_CALL:
        return "tool_call";
    case SC_REPLAY_TOOL_RESULT:
        return "tool_result";
    case SC_REPLAY_PROVIDER_REQUEST:
        return "provider_request";
    case SC_REPLAY_PROVIDER_RESPONSE:
        return "provider_response";
    case SC_REPLAY_USER_MESSAGE:
        return "user_message";
    case SC_REPLAY_AGENT_RESPONSE:
        return "agent_response";
    default:
        return "unknown";
    }
}

sc_error_t sc_replay_export_json(sc_replay_recorder_t *rec, sc_allocator_t *alloc, char **out_json,
                                 size_t *out_len) {
    if (!rec || !alloc || !out_json || !out_len)
        return SC_ERR_INVALID_ARGUMENT;

    size_t cap = 256 + rec->count * 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "{\"events\":[");
    for (size_t i = 0; i < rec->count && off < cap - 64; i++) {
        if (i > 0)
            buf[off++] = ',';
        sc_replay_event_t *e = &rec->events[i];
        off += (size_t)snprintf(buf + off, cap - off,
                                "{\"type\":\"%s\",\"timestamp\":%lld,\"data_len\":%zu}",
                                event_type_str(e->type), (long long)e->timestamp, e->data_len);
    }
    off += (size_t)snprintf(buf + off, cap - off, "]}");

    *out_json = buf;
    *out_len = off;
    return SC_OK;
}

size_t sc_replay_event_count(sc_replay_recorder_t *rec) {
    return rec ? rec->count : 0;
}

sc_error_t sc_replay_get_event(sc_replay_recorder_t *rec, size_t index, sc_replay_event_t *out) {
    if (!rec || !out || index >= rec->count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = rec->events[index];
    return SC_OK;
}
