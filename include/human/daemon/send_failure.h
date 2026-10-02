#ifndef HU_DAEMON_SEND_FAILURE_H
#define HU_DAEMON_SEND_FAILURE_H

/*
 * Undelivered-send recorder: what the daemon does when the iMessage channel
 * reports a text that failed on every path
 * (include/human/channels/imessage_send_observer.h).
 *
 * WHY (2026-09-26): a reply to an RCS contact failed three times (imsg timed
 * out, the AppleScript fallback could not reach a non-iMessage phone) and the
 * only trace was a debug-level "falling back" line. From her side the twin
 * ignored her. Now each final failure:
 *   1. writes a proactive_decisions row (trigger 'outbound_send', decision
 *      'send', reason 'send_failed', sent 0, a short message_ref prefix) —
 *      the table the proactive repeat guard and send circuit already read;
 *   2. logs one WARN line with a process counter (service + count, no text,
 *      no handle);
 *   3. notifies the owner (hu_owner_notify_local), at most once per contact
 *      per 10 minutes — a split reply fails bubble by bubble.
 *
 * A correctness fix, not a behaviour change to what is sent: ungated. It is
 * registered by hu_daemon_send_provenance_install, on the same memory.db.
 */

#include "human/channels/imessage_send_observer.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Final send failures recorded by this process (every build; 0 without
 * SQLite). The reply loop compares it around a send to tell a failed send
 * from a deliberate non-send (a bare tapback, the parrot guard). */
uint64_t hu_daemon_send_failure_total(void);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Record one final send failure (see above). `now` is unix seconds. */
void hu_daemon_send_failure_record(sqlite3 *db, const hu_imessage_send_failed_event_t *ev,
                                   int64_t now);

/* True when `contact`'s most recent final send failure is newer than its most
 * recent delivered send (outbound_sends); *out_ts gets the failure time. */
bool hu_daemon_send_failure_last_undelivered(sqlite3 *db, const char *contact, int64_t *out_ts);

#ifdef HU_IS_TEST
void hu_daemon_send_failure_test_reset(void);
#endif

#endif /* HU_ENABLE_SQLITE */

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_SEND_FAILURE_H */
