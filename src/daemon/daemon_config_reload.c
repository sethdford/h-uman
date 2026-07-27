/* src/daemon/daemon_config_reload.c
 *
 * Consumes the SIGHUP reload flag inside the daemon service loop. Lives beside
 * the other daemon carve-outs rather than in daemon.c (file-size ceiling
 * ratchet), and as a separate TU so the tick is callable from a test —
 * hu_service_run itself returns before its loop under HU_IS_TEST, so the loop
 * body is not reachable from the suite.
 *
 * The scope/threading contract is documented on the header; read it before
 * changing what this reloads. */

#include "human/daemon/config_reload.h"

#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"

#include <stddef.h>

#if !defined(_WIN32)
static pthread_mutex_t *g_reload_guard = NULL;

void hu_daemon_config_reload_set_guard(pthread_mutex_t *guard) {
    g_reload_guard = guard;
}
#endif

static void reload_guard_lock(void) {
#if !defined(_WIN32)
    if (g_reload_guard)
        pthread_mutex_lock(g_reload_guard);
#endif
}

static void reload_guard_unlock(void) {
#if !defined(_WIN32)
    if (g_reload_guard)
        pthread_mutex_unlock(g_reload_guard);
#endif
}

bool hu_daemon_config_reload_tick(struct hu_agent *agent, struct hu_observer *observer) {
    /* Cheap common path: one atomic exchange per service tick. */
    if (!hu_config_get_and_clear_reload_requested())
        return false;

    if (!agent) {
        hu_log_warn("human", observer,
                    "SIGHUP received but the service loop has no agent — nothing to reload");
        return false;
    }

    char *summary = NULL;
    size_t summary_len = 0;

    /* Serialize against the gateway bridge thread: hu_agent_reload_config
     * destroys and recreates agent->hook_registry, which a concurrent turn
     * reads in the tool gate. */
    reload_guard_lock();
    hu_error_t err = hu_agent_reload_config(agent, &summary, &summary_len);
    reload_guard_unlock();

    if (err != HU_OK) {
        hu_log_error("human", observer, "config reload failed: %s", hu_error_string(err));
        return false;
    }

    hu_log_info("human", observer, "config reloaded via SIGHUP");
    if (summary) {
        hu_log_info("human", observer, "%s", summary);
        if (agent->alloc)
            agent->alloc->free(agent->alloc->ctx, summary, summary_len + 1);
    }

    /* Emitted on EVERY reload, not once: an operator who just SIGHUP'd a
     * routing edit needs to see — at that moment — that the edit did not take.
     * A bare "config reloaded" line is precisely how an inert change reads as
     * an applied one. */
    hu_log_info("human", observer,
                "SIGHUP reload covers hooks, permission tier, and instruction files only — "
                "model routing, channels, feeds, and proactive settings are read from the "
                "daemon's startup config; restart the daemon to apply changes to those");

    return true;
}
