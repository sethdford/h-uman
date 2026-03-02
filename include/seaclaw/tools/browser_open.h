#ifndef SC_TOOLS_BROWSER_OPEN_H
#define SC_TOOLS_BROWSER_OPEN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_browser_open_create(sc_allocator_t *alloc,
    const char *const *allowed_domains, size_t allowed_count,
    sc_tool_t *out);

#endif
