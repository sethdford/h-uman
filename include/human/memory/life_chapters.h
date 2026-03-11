#ifndef HU_MEMORY_LIFE_CHAPTERS_H
#define HU_MEMORY_LIFE_CHAPTERS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/persona.h"
#include <stddef.h>

#ifdef HU_ENABLE_SQLITE

/* Get the active life chapter (most recent with active=1). */
hu_error_t hu_life_chapter_get_active(hu_allocator_t *alloc, hu_memory_t *memory,
                                     hu_life_chapter_t *out);

/* Store a new life chapter. Deactivates previous chapters and inserts with active=1. */
hu_error_t hu_life_chapter_store(hu_allocator_t *alloc, hu_memory_t *memory,
                                const hu_life_chapter_t *chapter);

/* Build directive string for prompt injection. Caller must free returned string. */
char *hu_life_chapter_build_directive(hu_allocator_t *alloc,
                                     const hu_life_chapter_t *chapter, size_t *out_len);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_LIFE_CHAPTERS_H */
