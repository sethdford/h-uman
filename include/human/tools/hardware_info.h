#ifndef HU_TOOLS_HARDWARE_INFO_H
#define HU_TOOLS_HARDWARE_INFO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_hardware_info_create(hu_allocator_t *alloc, bool enabled, hu_tool_t *out);

#endif
