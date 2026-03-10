#ifndef HU_CHANNELS_NOSTR_H
#define HU_CHANNELS_NOSTR_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_nostr_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count);

hu_error_t hu_nostr_create(hu_allocator_t *alloc, const char *nak_path, size_t nak_path_len,
                           const char *bot_pubkey, size_t bot_pubkey_len, const char *relay_url,
                           size_t relay_url_len, const char *seckey_hex, size_t seckey_len,
                           hu_channel_t *out);
void hu_nostr_destroy(hu_channel_t *ch);

/** Returns true if channel has relay and seckey configured (required for send). */
bool hu_nostr_is_configured(hu_channel_t *ch);

#if HU_IS_TEST
/** Test hook: get last message sent (caller must not free). */
const char *hu_nostr_test_last_message(hu_channel_t *ch);

/** Test hook: inject mock event for poll to return. */
hu_error_t hu_nostr_test_inject_mock_event(hu_channel_t *ch, const char *session_key,
                                           size_t session_key_len, const char *content,
                                           size_t content_len);
#endif

#endif /* HU_CHANNELS_NOSTR_H */
