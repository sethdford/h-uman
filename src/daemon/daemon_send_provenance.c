/* Send provenance recorder. Contract: include/human/daemon/send_provenance.h. */
#include "human/daemon/send_provenance.h"

#ifdef HU_ENABLE_SQLITE

#include "human/channels/imessage_send_observer.h"
#include "human/core/log.h"
#include "human/core/time.h"
#include "human/memory/outbound_sends_repo.h"
#include <stdatomic.h>

static void send_provenance_record(void *user, const hu_imessage_sent_event_t *ev) {
    sqlite3 *db = (sqlite3 *)user;
    hu_error_t err = hu_outbound_sends_repo_record(db, hu_time_get_current_ms(), "imessage",
                                                   ev->handle, ev->handle_len, ev->kind, ev->text,
                                                   ev->text_len, ev->prior_max_rowid);
    if (err != HU_OK) {
        /* Never fail the send over bookkeeping, but never go silent either:
         * a provenance log that stops writing makes the metric undercount
         * h-uman without any visible cause (reports-success-does-nothing.md). */
        static atomic_bool warned = false;
        hu_log_warn_once(&warned, "send_provenance", NULL,
                         "outbound_sends write failed (err=%d, kind=%s); send provenance is "
                         "incomplete until this clears",
                         (int)err, ev->kind ? ev->kind : "?");
    }
}

hu_error_t hu_daemon_send_provenance_install(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_outbound_sends_repo_ensure_schema(db);
    if (err != HU_OK)
        return err;
    hu_imessage_send_observer_set(send_provenance_record, db);
    static atomic_bool announced = false;
    hu_log_info_once(&announced, "send_provenance", NULL,
                     "send provenance active: recording delivered iMessage sends to "
                     "memory.db outbound_sends");
    return HU_OK;
}

void hu_daemon_send_provenance_uninstall(void) {
    hu_imessage_send_observer_set(NULL, NULL);
}

#endif /* HU_ENABLE_SQLITE */
