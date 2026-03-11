#ifndef HU_TOOLS_SAVE_FOR_LATER_H
#define HU_TOOLS_SAVE_FOR_LATER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/tool.h"

hu_error_t hu_save_for_later_create(hu_allocator_t *alloc, hu_memory_t *memory, hu_tool_t *out);

#endif
