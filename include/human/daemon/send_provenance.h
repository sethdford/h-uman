#ifndef HU_DAEMON_SEND_PROVENANCE_H
#define HU_DAEMON_SEND_PROVENANCE_H

/*
 * Send provenance: records every iMessage the daemon delivers into
 * memory.db `outbound_sends` (include/human/memory/outbound_sends_repo.h),
 * via the channel's send observer
 * (include/human/channels/imessage_send_observer.h).
 *
 * Logging only — it never changes or blocks a send, and a failed write is
 * logged once and dropped. So it is on by default with no OFF/SHADOW/LIVE
 * gate (feature-gate-requires-measurement.md applies to what gets SENT).
 */

#include "human/core/error.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the recorder. `db` must outlive the registration; call
 * hu_daemon_send_provenance_uninstall() before closing it. Replaces any
 * previously registered send observer. */
hu_error_t hu_daemon_send_provenance_install(sqlite3 *db);

/* Clear the recorder (idempotent). */
void hu_daemon_send_provenance_uninstall(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_DAEMON_SEND_PROVENANCE_H */
