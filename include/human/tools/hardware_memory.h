#ifndef HU_TOOLS_HARDWARE_MEMORY_H
#define HU_TOOLS_HARDWARE_MEMORY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_hardware_memory_create(hu_allocator_t *alloc, const char *const *boards,
                                     size_t boards_count, hu_tool_t *out);

#endif
