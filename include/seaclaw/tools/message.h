#ifndef SC_TOOLS_MESSAGE_H
#define SC_TOOLS_MESSAGE_H

#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_message_create(sc_allocator_t *alloc,
    sc_channel_t *channel, sc_tool_t *out);

#endif
