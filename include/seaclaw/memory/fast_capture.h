#ifndef SC_FAST_CAPTURE_H
#define SC_FAST_CAPTURE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory/stm.h"
#include <stdbool.h>
#include <stddef.h>

#define SC_FC_MAX_RESULTS 20

typedef struct sc_fc_entity_match {
    char *name;
    size_t name_len;
    char *type; /* "person", "date", "topic", "emotion" */
    size_t type_len;
    double confidence;
    size_t offset;
} sc_fc_entity_match_t;

typedef struct sc_fc_result {
    sc_fc_entity_match_t entities[SC_FC_MAX_RESULTS];
    size_t entity_count;
    sc_stm_emotion_t emotions[SC_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;
    bool has_commitment;
    bool has_question;
} sc_fc_result_t;

sc_error_t sc_fast_capture(sc_allocator_t *alloc, const char *text, size_t text_len,
                           sc_fc_result_t *out);
void sc_fc_result_deinit(sc_fc_result_t *result, sc_allocator_t *alloc);

#endif
