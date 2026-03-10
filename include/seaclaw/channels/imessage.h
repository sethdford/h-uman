#ifndef SC_CHANNELS_IMESSAGE_H
#define SC_CHANNELS_IMESSAGE_H

#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

sc_error_t sc_imessage_create(sc_allocator_t *alloc, const char *default_target,
                              size_t default_target_len, const char *const *allow_from,
                              size_t allow_from_count, sc_channel_t *out);
void sc_imessage_destroy(sc_channel_t *ch);

/** Returns true if default target (phone/email) is configured. */
bool sc_imessage_is_configured(sc_channel_t *ch);

/** Poll ~/Library/Messages/chat.db for new inbound messages (macOS only). */
sc_error_t sc_imessage_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                            size_t max_msgs, size_t *out_count);

/** Maps sc_reaction_type_t to iMessage tapback name (love, like, dislike, etc.).
 * Returns NULL for SC_REACTION_NONE or unknown. */
const char *sc_imessage_reaction_to_tapback_name(sc_reaction_type_t reaction);

#ifndef SC_IS_TEST
/** Check if the real user sent a message to `handle` within the last
 * `within_seconds` seconds.  Queries chat.db for is_from_me=1 rows.
 * Returns true if the user responded recently (SeaClaw should stay silent). */
bool sc_imessage_user_responded_recently(void *channel_ctx, const char *handle, size_t handle_len,
                                         int within_seconds);

/** Query the attachment path for a given message ROWID from chat.db.
 * Returns the attachment file path or NULL if not found. Caller owns. */
char *sc_imessage_get_attachment_path(sc_allocator_t *alloc, int64_t message_id);

/** Get the attachment path for the most recent message with an attachment from
 * the given contact. Returns path or NULL. Caller owns. */
char *sc_imessage_get_latest_attachment_path(sc_allocator_t *alloc, const char *contact_id,
                                             size_t contact_id_len);
#endif

#if SC_IS_TEST
sc_error_t sc_imessage_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                        size_t session_key_len, const char *content,
                                        size_t content_len);
const char *sc_imessage_test_get_last_message(sc_channel_t *ch, size_t *out_len);
void sc_imessage_test_get_last_reaction(sc_channel_t *ch, sc_reaction_type_t *out_reaction,
                                        int64_t *out_message_id);
#endif

#endif /* SC_CHANNELS_IMESSAGE_H */
