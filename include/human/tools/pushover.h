#ifndef HU_TOOLS_PUSHOVER_H
#define HU_TOOLS_PUSHOVER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_pushover_create(hu_allocator_t *alloc, const char *api_token, size_t api_token_len,
                              const char *user_key, size_t user_key_len, hu_tool_t *out);

#endif
