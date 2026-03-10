#ifndef HU_TOOLS_MESSAGE_H
#define HU_TOOLS_MESSAGE_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_message_create(hu_allocator_t *alloc, hu_channel_t *channel, hu_tool_t *out);

void hu_message_tool_set_channel(hu_tool_t *tool, hu_channel_t *channel);

#endif
