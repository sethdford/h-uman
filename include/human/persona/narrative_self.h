#ifndef HU_PERSONA_NARRATIVE_SELF_H
#define HU_PERSONA_NARRATIVE_SELF_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_NARRATIVE_MAX_THEMES         8
#define HU_NARRATIVE_MAX_ORIGIN_STORIES 4
#define HU_NARRATIVE_MAX_GROWTH_ARCS    6

typedef struct hu_narrative_self {
    char *identity_statement;
    char *themes[HU_NARRATIVE_MAX_THEMES];
    size_t theme_count;
    char *origin_stories[HU_NARRATIVE_MAX_ORIGIN_STORIES];
    size_t origin_count;
    char *growth_arcs[HU_NARRATIVE_MAX_GROWTH_ARCS];
    size_t growth_count;
    char *current_preoccupation;
    uint64_t last_reflection_ts;
} hu_narrative_self_t;

void hu_narrative_self_init(hu_narrative_self_t *self);
void hu_narrative_self_deinit(hu_allocator_t *alloc, hu_narrative_self_t *self);

hu_error_t hu_narrative_self_set_identity(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                          const char *statement, size_t len);

hu_error_t hu_narrative_self_add_theme(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                       const char *theme, size_t len);

hu_error_t hu_narrative_self_add_growth_arc(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                            const char *arc, size_t len);

hu_error_t hu_narrative_self_set_preoccupation(hu_allocator_t *alloc, hu_narrative_self_t *self,
                                               const char *text, size_t len);

hu_error_t hu_narrative_self_build_context(hu_allocator_t *alloc, const hu_narrative_self_t *self,
                                           char **out, size_t *out_len);

#endif
