#ifndef SC_CONTEXT_MOOD_H
#define SC_CONTEXT_MOOD_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stddef.h>

sc_error_t sc_mood_build_context(sc_allocator_t *alloc, sc_memory_t *memory,
                                 const char *contact_id, size_t contact_id_len,
                                 char **out, size_t *out_len);

#endif
