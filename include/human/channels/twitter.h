#ifndef HU_TWITTER_H
#define HU_TWITTER_H
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

hu_error_t hu_twitter_create(hu_allocator_t *alloc, const char *bearer_token,
                             size_t bearer_token_len, hu_channel_t *out);
void hu_twitter_destroy(hu_channel_t *ch);
hu_error_t hu_twitter_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                 size_t body_len);
hu_error_t hu_twitter_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                           size_t max_msgs, size_t *out_count);

#if HU_IS_TEST
hu_error_t hu_twitter_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len);
const char *hu_twitter_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif
#endif
