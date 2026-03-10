#ifndef HU_REPLAY_H
#define HU_REPLAY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_replay_event_type {
    HU_REPLAY_TOOL_CALL,
    HU_REPLAY_TOOL_RESULT,
    HU_REPLAY_PROVIDER_REQUEST,
    HU_REPLAY_PROVIDER_RESPONSE,
    HU_REPLAY_USER_MESSAGE,
    HU_REPLAY_AGENT_RESPONSE,
} hu_replay_event_type_t;

typedef struct hu_replay_event {
    hu_replay_event_type_t type;
    int64_t timestamp;
    char *data;
    size_t data_len;
} hu_replay_event_t;

typedef struct hu_replay_recorder hu_replay_recorder_t;

hu_replay_recorder_t *hu_replay_recorder_create(hu_allocator_t *alloc, uint32_t max_events);
void hu_replay_recorder_destroy(hu_replay_recorder_t *rec);

hu_error_t hu_replay_record(hu_replay_recorder_t *rec, hu_replay_event_type_t type,
                            const char *data, size_t data_len);

hu_error_t hu_replay_export_json(hu_replay_recorder_t *rec, hu_allocator_t *alloc, char **out_json,
                                 size_t *out_len);

size_t hu_replay_event_count(hu_replay_recorder_t *rec);

hu_error_t hu_replay_get_event(hu_replay_recorder_t *rec, size_t index, hu_replay_event_t *out);

#endif
