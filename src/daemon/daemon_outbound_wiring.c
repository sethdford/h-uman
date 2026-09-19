/* daemon_outbound_wiring.c — see daemon_outbound_wiring.h.
 *
 * The crosstalk registration and its teardown are moved VERBATIM from
 * src/daemon.c (init at the personal-model wiring block, teardown with the
 * personal-model teardown); only their location changed. The `sensitive`
 * registration is new. Comments are preserved so the original reasoning for
 * the SQLite ordering constraints does not get lost in the move.
 */

#include "human/daemon/daemon_outbound_wiring.h"

#include "human/agent.h"
#include "human/agent/outbound_sensitive.h"
#include "human/config.h"

#ifdef HU_ENABLE_SQLITE
#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/agent/reaction_handler.h"
#include "human/memory.h"
#endif

void hu_daemon_outbound_wiring_init(const struct hu_config *cfg, struct hu_agent *agent) {
#ifdef HU_ENABLE_SQLITE
    /* Sprint 60 follow-up — wire outbound crosstalk stage's cross-contact
     * bleed check to the production messages table. The stage already
     * shipped (Sprint 59 Phase B); without a registered lookup it runs
     * in degraded mode (metadata-pattern check only). Registration is
     * conditional on agent->memory being SQLite-backed; the corresponding
     * unregister sits in the teardown below so the static callback never
     * sees a freed sqlite3 *. */
    if (agent && agent->memory) {
        sqlite3 *crosstalk_db = hu_sqlite_memory_get_db(agent->memory);
        hu_outbound_crosstalk_register_sqlite(crosstalk_db);
        /* T8 (reflection retire-on-contradiction): wire the same SQLite
         * handle into the reaction handler so a thumbs_down retires the
         * reflection patterns that shaped the thumbed-down turn. Cleared
         * in the teardown below. */
        hu_reaction_handler_set_reflection_db(crosstalk_db);
    }
#else
    (void)agent;
#endif

    /* 2026-07-30 — the outbound sensitive-disclosure gate's protected values.
     * Without this the stage still blocks card / SSN / credential SHAPES (they
     * need no declared value), but the street-address rule stays inert: that
     * rule deliberately requires a match against the owner's OWN address so an
     * ordinary "meet me at 200 Central Ave" is never blocked. The stage logs
     * once when the set is empty, naming the config key to populate. */
    hu_outbound_sensitive_register_config(cfg);
}

void hu_daemon_outbound_wiring_teardown(void) {
#ifdef HU_ENABLE_SQLITE
    /* Sprint 60 follow-up teardown: clear the static crosstalk lookup
     * BEFORE the SQLite memory is closed so the callback never sees a
     * freed sqlite3 *. Idempotent — safe if registration didn't fire
     * (e.g. agent->memory was non-SQLite). */
    hu_outbound_crosstalk_unregister_sqlite();
    /* T8 teardown: clear the reaction handler's reflection-db borrow
     * before the SQLite memory closes so a late reaction never touches
     * a freed handle. */
    hu_reaction_handler_set_reflection_db(NULL);
#endif

    /* Stop borrowing the config arena: the provider is a process-wide static,
     * so leaving it registered past config teardown would let a late send scan
     * freed strings. */
    hu_outbound_sensitive_register_config(NULL);
}
