#ifndef HU_TOOLS_MEMORY_LIST_H
#define HU_TOOLS_MEMORY_LIST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_memory_list_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out);

#endif
