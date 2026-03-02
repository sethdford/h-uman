#ifndef SC_TOOLS_WEB_FETCH_H
#define SC_TOOLS_WEB_FETCH_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>
#include <stdint.h>

sc_error_t sc_web_fetch_create(sc_allocator_t *alloc,
    uint32_t max_chars, sc_tool_t *out);

#endif
