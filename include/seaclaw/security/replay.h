#ifndef SC_REPLAY_H
#define SC_REPLAY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_replay_event_type {
    SC_REPLAY_TOOL_CALL,
    SC_REPLAY_TOOL_RESULT,
    SC_REPLAY_PROVIDER_REQUEST,
    SC_REPLAY_PROVIDER_RESPONSE,
    SC_REPLAY_USER_MESSAGE,
    SC_REPLAY_AGENT_RESPONSE,
} sc_replay_event_type_t;

typedef struct sc_replay_event {
    sc_replay_event_type_t type;
    int64_t timestamp;
    char *data;
    size_t data_len;
} sc_replay_event_t;

typedef struct sc_replay_recorder sc_replay_recorder_t;

sc_replay_recorder_t *sc_replay_recorder_create(sc_allocator_t *alloc, uint32_t max_events);
void sc_replay_recorder_destroy(sc_replay_recorder_t *rec);

sc_error_t sc_replay_record(sc_replay_recorder_t *rec, sc_replay_event_type_t type,
                            const char *data, size_t data_len);

sc_error_t sc_replay_export_json(sc_replay_recorder_t *rec, sc_allocator_t *alloc, char **out_json,
                                 size_t *out_len);

size_t sc_replay_event_count(sc_replay_recorder_t *rec);

sc_error_t sc_replay_get_event(sc_replay_recorder_t *rec, size_t index, sc_replay_event_t *out);

#endif
