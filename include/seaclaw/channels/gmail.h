#ifndef SC_CHANNELS_GMAIL_H
#define SC_CHANNELS_GMAIL_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_gmail_create(sc_allocator_t *alloc, const char *client_id, size_t client_id_len,
                          const char *client_secret, size_t client_secret_len,
                          const char *refresh_token, size_t refresh_token_len, int poll_interval_sec,
                          sc_channel_t *out);

void sc_gmail_destroy(sc_channel_t *ch);

/** Returns true if OAuth credentials are configured. */
bool sc_gmail_is_configured(sc_channel_t *ch);

/** Poll Gmail API for unread messages; fills msgs, sets out_count. */
sc_error_t sc_gmail_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count);

#endif /* SC_CHANNELS_GMAIL_H */
