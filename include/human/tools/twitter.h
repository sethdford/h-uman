#ifndef HU_TOOLS_TWITTER_H
#define HU_TOOLS_TWITTER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_twitter_tool_create(hu_allocator_t *alloc, hu_tool_t *out);

#endif /* HU_TOOLS_TWITTER_H */
