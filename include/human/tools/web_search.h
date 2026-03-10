#ifndef HU_TOOLS_WEB_SEARCH_H
#define HU_TOOLS_WEB_SEARCH_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_web_search_create(hu_allocator_t *alloc, const hu_config_t *config,
                                const char *api_key, size_t api_key_len, hu_tool_t *out);

#endif
