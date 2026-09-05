/* src/channels/imessage_replay_guard.c — the pure replay guards from the
 * 2026-09-01 incident (stale-cursor replay of ~2,000 old iMessages).
 *
 * Separate from imessage.c on purpose: that file is compiled only under
 * HU_HAS_IMESSAGE (macOS), while tests/test_imessage_replay_guard.c is
 * unconditional — so on Linux CI (Benchmark, CodeQL, M3 smoke ubuntu) the
 * test binary failed to link with "undefined reference to
 * hu_imessage_resume_rowid" (2026-09-02). These four functions touch no
 * platform API; they belong in an always-built unit. */
#include "human/channels/imessage_replay_guard.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

int64_t hu_imessage_resume_rowid(int64_t persisted, int64_t db_max, int64_t max_replay,
                                 int64_t *out_skipped) {
    if (out_skipped)
        *out_skipped = 0;
    if (persisted <= 0 || persisted > db_max)
        return db_max;
    int64_t gap = db_max - persisted;
    if (max_replay >= 0 && gap > max_replay) {
        if (out_skipped)
            *out_skipped = gap;
        return db_max;
    }
    return persisted;
}

bool hu_imessage_inbound_is_stale(int64_t msg_unix_ts, int64_t now_unix, int64_t max_age_sec) {
    if (msg_unix_ts <= HU_IMESSAGE_APPLE_EPOCH_UNIX || max_age_sec <= 0)
        return false; /* unknown date (m.date == 0 → 2001-01-01) is never stale */
    return (now_unix - msg_unix_ts) > max_age_sec;
}

int64_t hu_imessage_parse_env_int64(const char *s, int64_t dflt) {
    if (!s || s[0] < '0' || s[0] > '9')
        return dflt; /* strtoll would skip leading whitespace / accept a sign; we don't */
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0)
        return dflt; /* partial parse, trailing junk, sign, or overflow → keep the default */
    return (int64_t)v;
}

bool hu_imessage_replied_guard_applies(const char *handle, const char *loopback_handle) {
    if (!handle || !handle[0])
        return false;
    if (!loopback_handle || !loopback_handle[0])
        return true;
    return strcasecmp(handle, loopback_handle) != 0;
}
