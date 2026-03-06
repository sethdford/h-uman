#ifndef SC_CHANNELS_FACEBOOK_H
#define SC_CHANNELS_FACEBOOK_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_facebook_create(sc_allocator_t *alloc, const char *page_id, size_t page_id_len,
                              const char *page_access_token, size_t token_len,
                              const char *app_secret, size_t secret_len, sc_channel_t *out);

sc_error_t sc_facebook_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                  size_t body_len);

sc_error_t sc_facebook_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count);

void sc_facebook_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_FACEBOOK_H */
