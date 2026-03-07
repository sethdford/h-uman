#ifndef SC_INSTAGRAM_H
#define SC_INSTAGRAM_H
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_instagram_create(sc_allocator_t *alloc, const char *business_account_id,
                               size_t business_account_id_len, const char *access_token,
                               size_t access_token_len, const char *app_secret,
                               size_t app_secret_len, sc_channel_t *out);
void sc_instagram_destroy(sc_channel_t *ch);
sc_error_t sc_instagram_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                   size_t body_len);
sc_error_t sc_instagram_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                             size_t max_msgs, size_t *out_count);

#if SC_IS_TEST
sc_error_t sc_instagram_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                         size_t session_key_len, const char *content,
                                         size_t content_len);
const char *sc_instagram_test_get_last_message(sc_channel_t *ch, size_t *out_len);
#endif
#endif
