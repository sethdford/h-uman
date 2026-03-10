#ifndef HU_TOOLS_WEB_FETCH_H
#define HU_TOOLS_WEB_FETCH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_web_fetch_create(hu_allocator_t *alloc, uint32_t max_chars, hu_tool_t *out);

#endif
