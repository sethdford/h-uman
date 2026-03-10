#ifndef HU_CHANNELS_TWILIO_H
#define HU_CHANNELS_TWILIO_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_twilio_create(hu_allocator_t *alloc, const char *account_sid, size_t account_sid_len,
                            const char *auth_token, size_t auth_token_len, const char *from_number,
                            size_t from_number_len, const char *to_number, size_t to_number_len,
                            hu_channel_t *out);

hu_error_t hu_twilio_on_webhook(void *channel_ctx, hu_allocator_t *alloc, const char *body,
                                size_t body_len);

hu_error_t hu_twilio_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count);

bool hu_twilio_is_configured(hu_channel_t *ch);

void hu_twilio_destroy(hu_channel_t *ch);

#if HU_IS_TEST
hu_error_t hu_twilio_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                      size_t session_key_len, const char *content,
                                      size_t content_len);
const char *hu_twilio_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif

#endif /* HU_CHANNELS_TWILIO_H */
