#ifndef HU_TOOL_ROUTER_H
#define HU_TOOL_ROUTER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

#define HU_TOOL_ROUTER_MAX_SELECTED 15

typedef struct hu_tool_selection {
    size_t indices[HU_TOOL_ROUTER_MAX_SELECTED];
    size_t count;
} hu_tool_selection_t;

hu_error_t hu_tool_router_select(hu_allocator_t *alloc, const char *message, size_t message_len,
                                  hu_tool_t *all_tools, size_t all_tools_count,
                                  hu_tool_selection_t *out);

#endif /* HU_TOOL_ROUTER_H */
