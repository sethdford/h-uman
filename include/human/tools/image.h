#ifndef HU_TOOLS_IMAGE_H
#define HU_TOOLS_IMAGE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_image_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                           hu_tool_t *out);

#endif
