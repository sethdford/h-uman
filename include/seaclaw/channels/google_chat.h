#ifndef SC_CHANNELS_GOOGLE_CHAT_H
#define SC_CHANNELS_GOOGLE_CHAT_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_google_chat_create(sc_allocator_t *alloc, const char *bearer_token,
                                 size_t bearer_token_len, const char *space_id,
                                 size_t space_id_len, sc_channel_t *out);

sc_error_t sc_google_chat_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                     size_t body_len);

sc_error_t sc_google_chat_poll(void *channel_ctx, sc_allocator_t *alloc,
                               sc_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count);

void sc_google_chat_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_GOOGLE_CHAT_H */
