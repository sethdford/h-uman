#ifndef SC_TOOLS_WEB_SEARCH_H
#define SC_TOOLS_WEB_SEARCH_H

#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_web_search_create(sc_allocator_t *alloc,
    const sc_config_t *config,
    const char *api_key, size_t api_key_len,
    sc_tool_t *out);

#endif
