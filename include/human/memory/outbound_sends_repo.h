#ifndef HU_MEMORY_OUTBOUND_SENDS_REPO_H
#define HU_MEMORY_OUTBOUND_SENDS_REPO_H

/*
 * Outbound-sends repository — one row per message the daemon actually
 * DELIVERED, so offline measurement can tell h-uman's chat.db rows from
 * Seth's own (both come from Seth's account). Fed by the iMessage send
 * observer via src/daemon/daemon_send_provenance.c; read by
 * scripts/eval_conversation_quality.py.
 *
 * Recall (memory) bounded context: the legal home for a raw sqlite3 include
 * (sqlite-includer-ratchet.md). Same free-function shape as
 * proactive_decisions_repo — one write path, one count.
 *
 * The chat.db row for a record is the first is_from_me row for `contact`
 * with ROWID > prior_max_rowid whose text matches; resolution happens
 * offline, so the send path never blocks on a chat.db read-back.
 */

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_OUTBOUND_SEND_KIND_TEXT  "text"
#define HU_OUTBOUND_SEND_KIND_MEDIA "media"
#define HU_OUTBOUND_SEND_KIND_REPLY "reply"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent CREATE TABLE/INDEX IF NOT EXISTS; cheap no-op after the first. */
hu_error_t hu_outbound_sends_repo_ensure_schema(sqlite3 *db);

/* Insert one delivered send.
 *   sent_at_ms      — unix ms when the channel reported success.
 *   channel         — non-empty, e.g. "imessage".
 *   contact         — (ptr, len) handle; need not be NUL-terminated; non-empty.
 *   kind            — one of HU_OUTBOUND_SEND_KIND_*; anything else is
 *                     HU_ERR_INVALID_ARGUMENT (a typo must not land as an
 *                     uncategorized row the metric then miscounts).
 *   text            — final delivered text; NULL/0 for media-only.
 *   prior_max_rowid — chat.db boundary read before the send; -1 unknown. */
hu_error_t hu_outbound_sends_repo_record(sqlite3 *db, int64_t sent_at_ms, const char *channel,
                                         const char *contact, size_t contact_len, const char *kind,
                                         const char *text, size_t text_len,
                                         int64_t prior_max_rowid);

hu_error_t hu_outbound_sends_repo_count(sqlite3 *db, int64_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_OUTBOUND_SENDS_REPO_H */
