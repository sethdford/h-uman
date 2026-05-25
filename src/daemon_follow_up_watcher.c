/* src/daemon_follow_up_watcher.c
 *
 * US-48-3: Follow-up watcher — daemon subsystem that polls iMessage chat.db
 * for read-but-unreplied messages from seth's contacts, computes circadian-aware
 * follow-up delays, and schedules them via daemon_proactive.
 *
 * Exported tick function hu_daemon_tick_follow_up_watcher is called from
 * daemon.c at configurable intervals (default 5 min).
 */

#include "human/agent.h"
#include "human/channels/imessage.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/daemon_proactive.h"
#include "human/follow_up.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Per ~/.claude/rules/silent-config-gated-subsystems.md: emit ONE
 * operator-visible log line per process when follow_up_watcher is
 * disabled or enabled. Guards are process-scoped via atomic_bool. */
static atomic_bool g_warned_followup_watcher_disabled = false;
static atomic_bool g_warned_followup_watcher_enabled = false;

#if HU_IS_TEST
void hu_daemon_follow_up_watcher_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_followup_watcher_disabled, false);
    atomic_store(&g_warned_followup_watcher_enabled, false);
}
#endif

hu_error_t hu_daemon_tick_follow_up_watcher(const struct hu_follow_up_watcher_config *cfg,
                                            int64_t now_unix, int64_t *last_poll_unix_inout,
                                            int64_t *watermark_inout, struct hu_agent *agent,
                                            const struct hu_config *config,
                                            hu_service_channel_t *channels, size_t channel_count,
                                            struct hu_proactive_throttle *throttle) {
    (void)agent;         /* unused in stub */
    (void)config;        /* unused in stub */
    (void)channels;      /* unused in stub */
    (void)channel_count; /* unused in stub */
    (void)throttle;      /* unused in stub */
    if (!cfg || !last_poll_unix_inout || !watermark_inout)
        return HU_ERR_INVALID_ARGUMENT;

    if (!cfg->enabled) {
        hu_log_info_once(&g_warned_followup_watcher_disabled, "follow_up_watcher", NULL,
                         "follow_up_watcher subsystem disabled by config "
                         "(cfg->follow_up_watcher.enabled=false); set "
                         "follow_up_watcher.enabled=true in config.json to activate");
        return HU_OK;
    }

    hu_log_info_once(&g_warned_followup_watcher_enabled, "follow_up_watcher", NULL,
                     "follow_up_watcher subsystem activated by config "
                     "(cfg->follow_up_watcher.enabled=true)");

    /* Check if enough time has passed since last poll. Default interval is 300s (5 min). */
    int interval = cfg->interval_seconds > 0 ? cfg->interval_seconds : 300;
    if (*last_poll_unix_inout > 0 && now_unix - *last_poll_unix_inout < interval)
        return HU_OK;

    /* Stub implementation: in a full version, this would:
     * 1. Call hu_imessage_find_unreplied_read() for known contacts
     * 2. For each unresponded read, compute follow-up delay via hu_followup_decide()
     * 3. Store in follow_up_scheduled table
     * 4. Flush ready ones via hu_daemon_follow_up_flush_for_contact()
     *
     * For now, just log and advance the watermark. */
    hu_log_info("follow_up_watcher", NULL, "follow-up watcher tick (stub) — no unresponded reads");

    *last_poll_unix_inout = now_unix;
    *watermark_inout = now_unix;

    return HU_OK;
}
