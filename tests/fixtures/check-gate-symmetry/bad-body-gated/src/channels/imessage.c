/* Fixture source: every code line is inside the platform macro, no stub. */
#include "human/channels/imessage.h"

#if HU_HAS_IMESSAGE

int64_t hu_imessage_resume_rowid(int64_t persisted, int64_t db_max, int64_t cap,
                                 int64_t *skipped_out) {
    (void)cap;
    if (skipped_out)
        *skipped_out = 0;
    return persisted ? persisted : db_max;
}

bool hu_imessage_inbound_is_stale(int64_t ts, int64_t now, int64_t window) {
    return window > 0 && now - ts > window;
}

#endif /* HU_HAS_IMESSAGE */
