#ifndef SC_CHANNELS_TWILIO_H
#define SC_CHANNELS_TWILIO_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_twilio_create(sc_allocator_t *alloc, const char *account_sid, size_t account_sid_len,
                            const char *auth_token, size_t auth_token_len, const char *from_number,
                            size_t from_number_len, sc_channel_t *out);

sc_error_t sc_twilio_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                size_t body_len);

sc_error_t sc_twilio_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count);

void sc_twilio_destroy(sc_channel_t *ch);

#endif /* SC_CHANNELS_TWILIO_H */
