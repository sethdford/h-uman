#ifndef HU_TOOLS_HTTP_REQUEST_H
#define HU_TOOLS_HTTP_REQUEST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_http_request_create(hu_allocator_t *alloc, bool allow_http, hu_tool_t *out);

#endif
