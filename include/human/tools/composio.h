#ifndef HU_TOOLS_COMPOSIO_H
#define HU_TOOLS_COMPOSIO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_composio_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                              const char *entity_id, size_t entity_id_len, hu_tool_t *out);

#endif
