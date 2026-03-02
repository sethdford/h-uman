#ifndef SC_TOOLS_HTTP_REQUEST_H
#define SC_TOOLS_HTTP_REQUEST_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_http_request_create(sc_allocator_t *alloc,
    bool allow_http, sc_tool_t *out);

#endif
