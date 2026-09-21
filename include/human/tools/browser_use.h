#ifndef HU_TOOLS_BROWSER_USE_H
#define HU_TOOLS_BROWSER_USE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/tool.h"

hu_error_t hu_browser_use_create(hu_allocator_t *alloc, hu_tool_t *out);
void hu_browser_use_destroy(hu_allocator_t *alloc, hu_tool_t *tool);

#endif /* HU_TOOLS_BROWSER_USE_H */
