#ifndef HU_CHANNELS_IMESSAGE_H
#define HU_CHANNELS_IMESSAGE_H

#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

hu_error_t hu_imessage_create(hu_allocator_t *alloc, const char *default_target,
                              size_t default_target_len, const char *const *allow_from,
                              size_t allow_from_count, hu_channel_t *out);
void hu_imessage_destroy(hu_channel_t *ch);

/** Returns true if default target (phone/email) is configured. */
bool hu_imessage_is_configured(hu_channel_t *ch);

/** Poll ~/Library/Messages/chat.db for new inbound messages (macOS only). */
hu_error_t hu_imessage_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count);

/** Maps hu_reaction_type_t to iMessage tapback name (love, like, dislike, etc.).
 * Returns NULL for HU_REACTION_NONE or unknown. */
const char *hu_imessage_reaction_to_tapback_name(hu_reaction_type_t reaction);

#ifndef HU_IS_TEST
/** Check if the real user sent a message to `handle` within the last
 * `within_seconds` seconds.  Queries chat.db for is_from_me=1 rows.
 * Returns true if the user responded recently (Human should stay silent). */
bool hu_imessage_user_responded_recently(void *channel_ctx, const char *handle, size_t handle_len,
                                         int within_seconds);

/** Query the attachment path for a given message ROWID from chat.db.
 * Returns the attachment file path or NULL if not found. Caller owns. */
char *hu_imessage_get_attachment_path(hu_allocator_t *alloc, int64_t message_id);

/** Get the attachment path for the most recent message with an attachment from
 * the given contact. Returns path or NULL. Caller owns. */
char *hu_imessage_get_latest_attachment_path(hu_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len);
#endif

#if HU_IS_TEST
hu_error_t hu_imessage_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len);
const char *hu_imessage_test_get_last_message(hu_channel_t *ch, size_t *out_len);
void hu_imessage_test_get_last_reaction(hu_channel_t *ch, hu_reaction_type_t *out_reaction,
                                        int64_t *out_message_id);
#endif

#endif /* HU_CHANNELS_IMESSAGE_H */
