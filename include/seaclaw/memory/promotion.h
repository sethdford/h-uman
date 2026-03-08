#ifndef SC_MEMORY_PROMOTION_H
#define SC_MEMORY_PROMOTION_H

#include "seaclaw/memory.h"
#include "seaclaw/memory/stm.h"

typedef struct sc_promotion_config {
    uint32_t min_mention_count;
    double min_importance;
    uint32_t max_entities;
} sc_promotion_config_t;

#define SC_PROMOTION_DEFAULTS {.min_mention_count = 2, .min_importance = 0.3, .max_entities = 15}

double sc_promotion_entity_importance(const sc_stm_entity_t *entity, const sc_stm_buffer_t *buf);
sc_error_t sc_promotion_run(sc_allocator_t *alloc, const sc_stm_buffer_t *buf, sc_memory_t *memory,
                            const sc_promotion_config_t *config);
sc_error_t sc_promotion_run_emotions(sc_allocator_t *alloc, const sc_stm_buffer_t *buf,
                                      sc_memory_t *memory, const char *contact_id,
                                      size_t contact_id_len);

#endif
