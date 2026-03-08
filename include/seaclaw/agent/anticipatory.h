#ifndef SC_ANTICIPATORY_H
#define SC_ANTICIPATORY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory/graph.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_ANTICIPATORY_MAX_ACTIONS 8

typedef enum sc_action_type {
    SC_ACTION_FOLLOW_UP, /* check in on something mentioned earlier */
    SC_ACTION_REMIND,    /* remind about a commitment */
    SC_ACTION_CELEBRATE, /* acknowledge an upcoming event */
    SC_ACTION_EMPATHIZE, /* check in on emotional state */
} sc_action_type_t;

typedef struct sc_anticipatory_action {
    sc_action_type_t type;
    char *description;
    size_t description_len;
    char *trigger_entity;
    size_t trigger_entity_len;
    float relevance;        /* 0.0-1.0 */
    int64_t suggested_time; /* when to trigger */
} sc_anticipatory_action_t;

typedef struct sc_anticipatory_result {
    sc_anticipatory_action_t actions[SC_ANTICIPATORY_MAX_ACTIONS];
    size_t action_count;
} sc_anticipatory_result_t;

/* Analyze graph for anticipatory actions for a contact */
sc_error_t sc_anticipatory_analyze(sc_graph_t *graph, sc_allocator_t *alloc, const char *contact_id,
                                   size_t contact_id_len, int64_t now_ts,
                                   sc_anticipatory_result_t *result);

/* Build prompt context from anticipatory actions */
sc_error_t sc_anticipatory_build_context(const sc_anticipatory_result_t *result,
                                         sc_allocator_t *alloc, char **out, size_t *out_len);

/* Free result */
void sc_anticipatory_result_deinit(sc_anticipatory_result_t *result, sc_allocator_t *alloc);

#endif
