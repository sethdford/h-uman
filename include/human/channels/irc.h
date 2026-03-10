#ifndef HU_CHANNELS_IRC_H
#define HU_CHANNELS_IRC_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_irc_create(hu_allocator_t *alloc, const char *server, size_t server_len,
                         uint16_t port, hu_channel_t *out);

hu_error_t hu_irc_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                       size_t max_msgs, size_t *out_count);

void hu_irc_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_irc_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                   size_t session_key_len, const char *content, size_t content_len);
const char *hu_irc_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_IRC_H */ /* HU_CHANNELS_IRC_H */
