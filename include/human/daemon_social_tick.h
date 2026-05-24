/* include/human/daemon_social_tick.h
 *
 * Sprint A.6 wire: periodic daemon tick that exercises three Tier-2
 * libraries currently sitting library-only (no production caller):
 *   - hu_imessage_scan_stale_contacts (gap detection)
 *   - hu_drift_scan_top_contacts (pattern drift)
 *   - hu_contact_signature_top_n (per-contact signatures)
 *
 * Output: a one-shot JSON snapshot at ~/.human/social_state.json that
 * the persona prompt can read on next ingest cycle, the PWA can
 * surface, and operators can `cat` to inspect.
 *
 * Cadence: every 6 hours (configurable via cfg->social_tick_interval_seconds).
 * Cost: ~3 chat.db scans per tick, all read-only, bounded by top-N caps.
 *
 * Under HU_IS_TEST: returns OK without doing real I/O (the underlying
 * scanners themselves are gated; this layer just adds the tick + JSON
 * emission). */

#ifndef HU_DAEMON_SOCIAL_TICK_H
#define HU_DAEMON_SOCIAL_TICK_H

#include "human/config.h"
#include "human/core/error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single tick: pulls signatures + drift + gaps for top-N contacts and
 * writes the result to `out_path` as JSON. NULL `out_path` defaults to
 * ~/.human/social_state.json. Caller passes `now_unix` for testability. */
hu_error_t hu_daemon_social_tick_run(const hu_config_t *cfg, const char *db_path,
                                     const char *out_path, int64_t now_unix, size_t top_n);

/* Interval-gated tick used by the daemon main loop. Default interval
 * 6 hours; overridden by cfg->social_state.tick_interval_seconds if set. */
hu_error_t hu_daemon_social_tick(const hu_config_t *cfg, int64_t now_unix,
                                 int64_t *last_run_unix_inout);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_SOCIAL_TICK_H */
