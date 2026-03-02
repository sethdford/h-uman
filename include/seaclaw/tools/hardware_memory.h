#ifndef SC_TOOLS_HARDWARE_MEMORY_H
#define SC_TOOLS_HARDWARE_MEMORY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_hardware_memory_create(sc_allocator_t *alloc,
    const char *const *boards, size_t boards_count,
    sc_tool_t *out);

#endif
