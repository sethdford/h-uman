#ifndef HU_CONTEXT_MOOD_H
#define HU_CONTEXT_MOOD_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

hu_error_t hu_mood_build_context(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const char *contact_id, size_t contact_id_len,
                                 char **out, size_t *out_len);

#endif
