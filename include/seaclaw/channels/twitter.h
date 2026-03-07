#ifndef SC_TWITTER_H
#define SC_TWITTER_H
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_twitter_create(sc_allocator_t *alloc, const char *bearer_token,
                             size_t bearer_token_len, sc_channel_t *out);
void sc_twitter_destroy(sc_channel_t *ch);
sc_error_t sc_twitter_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                 size_t body_len);
sc_error_t sc_twitter_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                           size_t max_msgs, size_t *out_count);

#if SC_IS_TEST
sc_error_t sc_twitter_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len);
const char *sc_twitter_test_get_last_message(sc_channel_t *ch, size_t *out_len);
#endif
#endif
