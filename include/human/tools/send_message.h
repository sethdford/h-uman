#ifndef HU_TOOLS_SEND_MESSAGE_H
#define HU_TOOLS_SEND_MESSAGE_H

#include "human/agent/mailbox.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"

hu_error_t hu_send_message_create(hu_allocator_t *alloc, hu_mailbox_t *mailbox, hu_tool_t *out);

#endif
