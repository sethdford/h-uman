#ifndef SC_TOOLS_MEMORY_STORE_H
#define SC_TOOLS_MEMORY_STORE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_memory_store_create(sc_allocator_t *alloc,
    sc_memory_t *memory, sc_tool_t *out);

#endif
