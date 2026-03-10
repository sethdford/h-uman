#ifndef HU_CHANNELS_LARK_H
#define HU_CHANNELS_LARK_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

hu_error_t hu_lark_create(hu_allocator_t *alloc, const char *app_id, size_t app_id_len,
                          const char *app_secret, size_t app_secret_len, hu_channel_t *out);

hu_error_t hu_lark_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                              size_t body_len);

hu_error_t hu_lark_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count);

bool hu_lark_is_configured(hu_channel_t *ch);
void hu_lark_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_lark_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                    size_t session_key_len, const char *content,
                                    size_t content_len);
const char *hu_lark_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_LARK_H */
