#ifndef SC_CHANNELS_NOSTR_H
#define SC_CHANNELS_NOSTR_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_nostr_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count);

sc_error_t sc_nostr_create(sc_allocator_t *alloc, const char *nak_path, size_t nak_path_len,
                           const char *bot_pubkey, size_t bot_pubkey_len, const char *relay_url,
                           size_t relay_url_len, const char *seckey_hex, size_t seckey_len,
                           sc_channel_t *out);
void sc_nostr_destroy(sc_channel_t *ch);

/** Returns true if channel has relay and seckey configured (required for send). */
bool sc_nostr_is_configured(sc_channel_t *ch);

#if SC_IS_TEST
/** Test hook: get last message sent (caller must not free). */
const char *sc_nostr_test_last_message(sc_channel_t *ch);

/** Test hook: inject mock event for poll to return. */
sc_error_t sc_nostr_test_inject_mock_event(sc_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len);
#endif

#endif /* SC_CHANNELS_NOSTR_H */
