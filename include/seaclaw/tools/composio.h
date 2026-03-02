#ifndef SC_TOOLS_COMPOSIO_H
#define SC_TOOLS_COMPOSIO_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_composio_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *entity_id, size_t entity_id_len,
    sc_tool_t *out);

#endif
