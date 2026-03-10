#ifndef HU_TOOLS_HOMEASSISTANT_H
#define HU_TOOLS_HOMEASSISTANT_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
hu_error_t hu_homeassistant_create(hu_allocator_t *alloc, hu_tool_t *out);
#endif
