#ifndef SC_CHANNELS_WHATSAPP_H
#define SC_CHANNELS_WHATSAPP_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_whatsapp_create(sc_allocator_t *alloc, const char *phone_number_id,
                              size_t phone_number_id_len, const char *token, size_t token_len,
                              sc_channel_t *out);

sc_error_t sc_whatsapp_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                  size_t body_len);

sc_error_t sc_whatsapp_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count);

void sc_whatsapp_destroy(sc_channel_t *ch);

#if SC_IS_TEST
sc_error_t sc_whatsapp_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len);
const char *sc_whatsapp_test_get_last_message(sc_channel_t *ch, size_t *out_len);
#endif

#endif /* SC_CHANNELS_WHATSAPP_H */
