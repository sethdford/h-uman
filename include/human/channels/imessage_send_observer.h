#ifndef HU_CHANNELS_IMESSAGE_SEND_OBSERVER_H
#define HU_CHANNELS_IMESSAGE_SEND_OBSERVER_H

/*
 * Send-provenance observer for the iMessage channel.
 *
 * Every outbound iMessage the daemon DELIVERS — plain text and media through
 * imessage_send, threaded replies through hu_imessage_reply — is reported
 * here exactly once, after the send succeeded, with the final text as it
 * went to chat.db (after overlay, markdown strip and sanitize). A send that
 * failed or was held reports nothing.
 *
 * Why: h-uman and Seth both send from Seth's account, so chat.db alone can't
 * say who wrote a message. Text-matching memory.db assistant rows misses
 * split, restyled and proactive/scheduled sends (2026-09-25 probe: 99 of 157
 * logged replies matched). The daemon registers an observer that records each
 * event (src/daemon/daemon_send_provenance.c), and the offline metric
 * resolves the chat.db row as the first is_from_me row for the handle with
 * ROWID > prior_max_rowid.
 *
 * A separate shared header (not imessage.h) so both imessage.c and
 * imessage_reply.c can report without a new cross-file channel include
 * (scripts/check-edge-context-isolation.sh exempts it as iMessage shared infra).
 *
 * Process-wide, one observer. Set it at startup before any sends; the
 * observer runs on the sending thread and must not block for long.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_IMESSAGE_SENT_KIND_TEXT  "text"
#define HU_IMESSAGE_SENT_KIND_MEDIA "media"
#define HU_IMESSAGE_SENT_KIND_REPLY "reply"

typedef struct hu_imessage_sent_event {
    const char *handle; /* recipient handle; (ptr, len), not NUL-terminated */
    size_t handle_len;
    const char *text; /* final delivered text; NULL/0 for media-only */
    size_t text_len;
    const char *kind;        /* one of HU_IMESSAGE_SENT_KIND_* */
    int64_t prior_max_rowid; /* chat.db MAX(ROWID) boundary before the send; -1 unknown */
} hu_imessage_sent_event_t;

typedef void (*hu_imessage_send_observer_fn)(void *user, const hu_imessage_sent_event_t *ev);

#ifdef __cplusplus
extern "C" {
#endif

/* Register (fn != NULL) or clear (fn == NULL) the process-wide observer. */
void hu_imessage_send_observer_set(hu_imessage_send_observer_fn fn, void *user);

/* True when an observer is registered — lets the send path skip the extra
 * chat.db boundary read entirely when nobody is listening. */
bool hu_imessage_send_observer_active(void);

/* Report one delivered send. No-op when no observer is set, ev is NULL, or
 * the handle is empty. */
void hu_imessage_send_observer_notify(const hu_imessage_sent_event_t *ev);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNELS_IMESSAGE_SEND_OBSERVER_H */
