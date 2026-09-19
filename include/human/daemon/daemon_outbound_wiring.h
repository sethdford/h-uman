#ifndef HU_DAEMON_OUTBOUND_WIRING_H
#define HU_DAEMON_OUTBOUND_WIRING_H

/* Outbound-stage data-source wiring, lifted out of daemon.c.
 *
 * Several outbound pipeline stages cannot reach their data source from inside
 * the pipeline, so each exposes a registration hook that startup must call.
 * Those calls were accumulating inline in daemon.c; they are collected here so
 * the set of "stages that need wiring" is visible in one place and the
 * god-file stops growing (.claude/rules/file-size-ceiling.md — the ratchet
 * exists to push exactly this kind of addition out of src/daemon.c).
 *
 * Currently wired:
 *   crosstalk  — cross-contact bleed lookup over the SQLite messages table
 *                (plus the T8 reflection-db borrow that shares the handle)
 *   sensitive  — the owner's protected values from the `privacy` config block
 *
 * Both registrations install PROCESS-WIDE statics that borrow memory owned by
 * the caller (a sqlite3 * and the config arena), so teardown ordering matters:
 * call the teardown before closing the SQLite memory or deinitializing the
 * config, or a late send could touch freed memory.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;
struct hu_config;

/* Register every outbound stage's data source. Safe with NULL arguments —
 * each stage degrades on its own terms (crosstalk to metadata-only, sensitive
 * to shape-only) and logs that it did. */
void hu_daemon_outbound_wiring_init(const struct hu_config *cfg, struct hu_agent *agent);

/* Clear all registrations. Idempotent; safe if init never fired. MUST run
 * before the SQLite memory closes and before hu_config_deinit. */
void hu_daemon_outbound_wiring_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_OUTBOUND_WIRING_H */
