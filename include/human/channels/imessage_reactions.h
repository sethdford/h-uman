#ifndef HU_IMESSAGE_REACTIONS_H
#define HU_IMESSAGE_REACTIONS_H

#include "human/channels/reaction_event.h"
#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads tapback rows from chat.db inserted since since_unix.
 * Fills out[0..min(cap, returned)] with events; sets *out_n.
 * Returns HU_ERR_NOT_SUPPORTED on non-Apple/test/missing SQLite.
 * Returns HU_ERR_IO on chat.db open/prepare failure. */
hu_error_t hu_imessage_poll_reactions(const char *db_path, int64_t since_unix,
                                      hu_reaction_event_t *out, size_t cap, size_t *out_n);

/* After an outbound send, resolve the latest is_from_me message GUID in chat.db
 * for correlation with tapback reactions. `text_prefix` may be NULL (any text).
 * Returns HU_ERR_NOT_FOUND when no row matches, HU_ERR_NOT_SUPPORTED off Apple. */
hu_error_t hu_imessage_lookup_latest_sent_guid(const char *db_path, const char *chat_guid,
                                              const char *text_prefix, char *out_guid,
                                              size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_IMESSAGE_REACTIONS_H */
