#ifndef HUMAN_CHANNELS_IMESSAGE_PRIVATE_CLIENT_H
#define HUMAN_CHANNELS_IMESSAGE_PRIVATE_CLIENT_H

/* Daemon-side client for the IMCore helper dylib (Phase 3).
 *
 * The daemon plays the role imessage-rs's Rust core plays: it binds the
 * per-user localhost port, the injected dylib connects back, and the daemon
 * sends JSON action commands. This header exposes:
 *   - a PURE command-string builder (the wire contract the Swift IMHelper
 *     dispatch parses) — fully unit-testable; and
 *   - a backend-selection predicate (LIVE + connected ⇒ use private API).
 * Socket/spawn lifecycle lives in client.c / inject.c behind HU_IS_TEST guards.
 *
 * Wire contract (must match apps/imessage-helper IMHelper.handleMessage):
 *   send/reply : {"action":"send-message","data":{"chatGuid","message"[,
 *                 "selectedMessageGuid","partIndex"]},"transactionId":"…"}
 *   tapback    : {"action":"send-reaction","data":{"chatGuid",
 *                 "selectedMessageGuid","reactionType","partIndex"},…}
 *   edit       : {"action":"edit-message","data":{"chatGuid","messageGuid",
 *                 "editedMessage","backwardsCompatibilityMessage","partIndex"},…}
 *   unsend     : {"action":"unsend-message","data":{"chatGuid","messageGuid",
 *                 "partIndex"},…}
 */

#include "human/channels/imessage_private/protocol.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Build the JSON command line (NO trailing newline) for a "send-message".
 * When parent_guid is non-NULL/non-empty, a threaded reply is requested
 * (selectedMessageGuid). text and chat_guid are JSON-string-escaped.
 * Writes a NUL-terminated string into out (cap bytes); returns HU_OK, or
 * HU_ERR_INVALID_ARGUMENT on NULL/empty required args, or HU_ERR_BUFFER_TOO_SMALL
 * if the result doesn't fit. */
hu_error_t hu_imessage_private_build_send(char *out, size_t cap, const char *txn_id,
                                          const char *chat_guid, const char *text,
                                          const char *parent_guid, int part_index);

/* Build the JSON command line for a classic tapback ("send-reaction").
 * reaction_type is one of love/like/dislike/laugh/emphasize/question, optionally
 * prefixed with '-' to remove. */
hu_error_t hu_imessage_private_build_reaction(char *out, size_t cap, const char *txn_id,
                                              const char *chat_guid, const char *parent_guid,
                                              const char *reaction_type, int part_index);

/* Build the JSON command line for an edit ("edit-message"). */
hu_error_t hu_imessage_private_build_edit(char *out, size_t cap, const char *txn_id,
                                          const char *chat_guid, const char *message_guid,
                                          const char *edited_text, const char *backcompat_text,
                                          int part_index);

/* Build the JSON command line for an unsend ("unsend-message"). */
hu_error_t hu_imessage_private_build_unsend(char *out, size_t cap, const char *txn_id,
                                            const char *chat_guid, const char *message_guid,
                                            int part_index);

/* Backend-selection predicate: should the dispatcher route this iMessage action
 * through the private-API backend rather than the Tier-1 fallback?
 * True only when the config gate is enabled, the mode is LIVE, AND the helper
 * dylib is currently connected. SHADOW/OFF, or not-connected → false (fall back
 * to the hardened Tier-1 path). Pure; no I/O. */
bool hu_imessage_private_should_route(bool enabled, hu_imessage_private_mode_t mode,
                                      bool helper_connected);

#endif /* HUMAN_CHANNELS_IMESSAGE_PRIVATE_CLIENT_H */
