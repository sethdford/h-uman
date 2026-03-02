#ifndef SC_TOOLS_IMAGE_H
#define SC_TOOLS_IMAGE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_image_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len, sc_tool_t *out);

#endif
