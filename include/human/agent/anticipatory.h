#ifndef HU_ANTICIPATORY_H
#define HU_ANTICIPATORY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_ANTICIPATORY_MAX_ACTIONS 8

typedef enum hu_action_type {
    HU_ACTION_FOLLOW_UP, /* check in on something mentioned earlier */
    HU_ACTION_REMIND,    /* remind about a commitment */
    HU_ACTION_CELEBRATE, /* acknowledge an upcoming event */
    HU_ACTION_EMPATHIZE, /* check in on emotional state */
} hu_action_type_t;

typedef struct hu_anticipatory_action {
    hu_action_type_t type;
    char *description;
    size_t description_len;
    char *trigger_entity;
    size_t trigger_entity_len;
    float relevance;        /* 0.0-1.0 */
    int64_t suggested_time; /* when to trigger */
} hu_anticipatory_action_t;

typedef struct hu_anticipatory_result {
    hu_anticipatory_action_t actions[HU_ANTICIPATORY_MAX_ACTIONS];
    size_t action_count;
} hu_anticipatory_result_t;

/* Analyze graph for anticipatory actions for a contact */
hu_error_t hu_anticipatory_analyze(hu_graph_t *graph, hu_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, int64_t now_ts,
                                   hu_anticipatory_result_t *result);

/* Build prompt context from anticipatory actions */
hu_error_t hu_anticipatory_build_context(const hu_anticipatory_result_t *result,
                                         hu_allocator_t *alloc, char **out, size_t *out_len);

/* Free result */
void hu_anticipatory_result_deinit(hu_anticipatory_result_t *result, hu_allocator_t *alloc);

#endif
