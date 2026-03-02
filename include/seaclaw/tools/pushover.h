#ifndef SC_TOOLS_PUSHOVER_H
#define SC_TOOLS_PUSHOVER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_pushover_create(sc_allocator_t *alloc,
    const char *api_token, size_t api_token_len,
    const char *user_key, size_t user_key_len,
    sc_tool_t *out);

#endif
