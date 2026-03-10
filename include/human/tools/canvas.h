#ifndef HU_TOOLS_CANVAS_H
#define HU_TOOLS_CANVAS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"

hu_error_t hu_canvas_create(hu_allocator_t *alloc, hu_tool_t *out);

#endif
