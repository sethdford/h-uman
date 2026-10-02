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

#ifdef HU_ENABLE_SQLITE
struct sqlite3;
/* Same query as hu_imessage_poll_reactions against an already-open chat.db
 * handle (fixture-testable). Also resolves the reacted-to message:
 * target_is_ours / target_sent_unix (see hu_reaction_event_t). */
hu_error_t hu_imessage_poll_reactions_db(struct sqlite3 *db, int64_t since_unix,
                                         hu_reaction_event_t *out, size_t cap, size_t *out_n);
#endif

/* After an outbound send, resolve the latest is_from_me message GUID in chat.db
 * for correlation with tapback reactions. `chat_guid` may be either a full
 * chat.guid ("any;-;+15551234") or the bare handle ("+15551234") — the daemon's
 * reply router carries the bare form, so both are accepted.
 * `text_prefix` may be NULL (any text).
 * Returns HU_ERR_NOT_FOUND when no row matches, HU_ERR_NOT_SUPPORTED off Apple. */
hu_error_t hu_imessage_lookup_latest_sent_guid(const char *db_path, const char *chat_guid,
                                               const char *text_prefix, char *out_guid,
                                               size_t out_cap);

/* ── Reaction-lookup join-key normalizers ────────────────────────────────────
 *
 * The reaction_lookup store is an exact-match join on (channel, thread,
 * msg_ref) between two INDEPENDENT producers:
 *
 *   registration  (daemon reply router)  — carries the daemon's thread id
 *   lookup        (chat.db tapback poll) — carries chat.guid / assoc guid
 *
 * Those two sources spell the same conversation and the same message
 * differently, so both sides MUST be pushed through these predicates before
 * they touch the store. Pure string functions: no DB, no allocation, safe to
 * unit-test in isolation (see .claude/rules/security-predicate-extraction.md).
 *
 * Both write a NUL-terminated result into `out` (truncating at `cap`) and
 * return HU_OK; a NULL/empty input yields an empty string. They are
 * idempotent — normalizing an already-normalized key is a no-op, which is
 * what makes them safe to apply at every boundary. */

/* chat.db `associated_message_guid` carries a "p:<part>/" prefix identifying
 * which part of a multipart message was reacted to ("p:0/", "p:1/", "p:4/"
 * all occur in real data; ~10% of rows carry no prefix at all). The part
 * index is not part of message identity for training purposes — a tapback on
 * any part is a reaction to the message — so it is stripped.
 * "bp:<GUID>" (tapback on a balloon / link preview) is stripped the same way.
 *   "p:0/ABC-123" -> "ABC-123"      "ABC-123" -> "ABC-123"   "bp:ABC" -> "ABC" */
hu_error_t hu_imessage_strip_assoc_guid_prefix(const char *raw, char *out, size_t cap);

/* chat.db `chat.guid` is "<service>;<kind>;<id>" ("any;-;+15551234" for DMs,
 * "any;+;chat9755..." for groups); the daemon reply router instead carries the
 * bare `<id>`. Reduce both to the bare id so they join.
 *   "any;-;+15551234" -> "+15551234"    "+15551234" -> "+15551234" */
hu_error_t hu_imessage_normalize_thread_key(const char *raw, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_IMESSAGE_REACTIONS_H */
