#ifndef SC_CHANNELS_LINE_H
#define SC_CHANNELS_LINE_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_line_create(sc_allocator_t *alloc, const char *channel_token,
                          size_t channel_token_len, sc_channel_t *out);

sc_error_t sc_line_create_ex(sc_allocator_t *alloc, const char *channel_token,
                             size_t channel_token_len, const char *user_id, size_t user_id_len,
                             sc_channel_t *out);

bool sc_line_is_configured(sc_channel_t *ch);

sc_error_t sc_line_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                              size_t body_len);

sc_error_t sc_line_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count);

void sc_line_destroy(sc_channel_t *ch);

#if SC_IS_TEST
sc_error_t sc_line_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                    size_t session_key_len, const char *content,
                                    size_t content_len);
const char *sc_line_test_get_last_message(sc_channel_t *ch, size_t *out_len);
#endif

#endif /* SC_CHANNELS_LINE_H */
