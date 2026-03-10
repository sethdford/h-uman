#ifndef HU_MEMORY_PROMOTION_H
#define HU_MEMORY_PROMOTION_H

#include "human/memory.h"
#include "human/memory/stm.h"

typedef struct hu_promotion_config {
    uint32_t min_mention_count;
    double min_importance;
    uint32_t max_entities;
} hu_promotion_config_t;

#define HU_PROMOTION_DEFAULTS {.min_mention_count = 2, .min_importance = 0.3, .max_entities = 15}

double hu_promotion_entity_importance(const hu_stm_entity_t *entity, const hu_stm_buffer_t *buf);
hu_error_t hu_promotion_run(hu_allocator_t *alloc, const hu_stm_buffer_t *buf, hu_memory_t *memory,
                            const hu_promotion_config_t *config);
hu_error_t hu_promotion_run_emotions(hu_allocator_t *alloc, const hu_stm_buffer_t *buf,
                                      hu_memory_t *memory, const char *contact_id,
                                      size_t contact_id_len);

#endif
