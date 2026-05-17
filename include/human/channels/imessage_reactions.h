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

#ifdef __cplusplus
}
#endif

#endif /* HU_IMESSAGE_REACTIONS_H */
