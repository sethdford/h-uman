#ifndef HU_FAST_CAPTURE_H
#define HU_FAST_CAPTURE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/stm.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_FC_MAX_RESULTS 20

typedef struct hu_fc_entity_match {
    char *name;
    size_t name_len;
    char *type; /* "person", "date", "topic", "emotion" */
    size_t type_len;
    double confidence;
    size_t offset;
} hu_fc_entity_match_t;

typedef struct hu_fc_result {
    hu_fc_entity_match_t entities[HU_FC_MAX_RESULTS];
    size_t entity_count;
    hu_stm_emotion_t emotions[HU_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;
    bool has_commitment;
    bool has_question;
} hu_fc_result_t;

hu_error_t hu_fast_capture(hu_allocator_t *alloc, const char *text, size_t text_len,
                           hu_fc_result_t *out);
void hu_fc_result_deinit(hu_fc_result_t *result, hu_allocator_t *alloc);

hu_error_t hu_fast_capture_data_init(hu_allocator_t *alloc);
void hu_fast_capture_data_cleanup(hu_allocator_t *alloc);

#endif
