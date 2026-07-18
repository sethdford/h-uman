/* src/daemon/daemon_learning_tick.c
 *
 * Learning-loop wiring for the daemon main loop:
 *
 *  - Proactive-outcome pipeline (US-104): record proactive sends into
 *    proactive_sends, resolve REPLY outcomes from inbound traffic, and
 *    periodically feed resolved outcomes into the humanization bandit
 *    (hu_proactive_outcomes_process_async moves the Beta(α,β) arms and
 *    persists them). Before 2026-07-18 all three dpo.c functions had
 *    zero callers — the bandit read path consulted arms that never moved.
 *
 *  - hu_daemon_dpo_judge_tick: 24h DPO judge cadence, carved verbatim
 *    from daemon.c to respect the file-size ceiling ratchet.
 */

#include "human/daemon_learning_tick.h"

#include "human/agent.h"
#include "human/agent/contextual_bandit.h"
#include "human/core/log.h"
#include "human/ml/dpo.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

/* NOTE: no direct <sqlite3.h> include (sqlite-includer-ratchet.md) — the
 * sqlite3 handle type comes from human/memory.h's get_db declaration, and
 * all SQL lives in src/ml/dpo.c. This module only passes the handle. */

/* Per .claude/rules/silent-config-gated-subsystems.md: one operator-visible
 * line per process for inactive vs active, so "the bandit is silently not
 * learning" (sota off, bandit creation failed) is a discoverable fact in
 * the service log rather than a runtime mystery. */
static atomic_bool g_outcome_warned_inactive = false;
static atomic_bool g_outcome_warned_active = false;
static int64_t g_outcome_last_tick = 0;

#define HU_PROACTIVE_OUTCOME_TICK_INTERVAL_S 60

#ifdef HU_IS_TEST
void hu_daemon_learning_tick_reset_for_test(void) {
    atomic_store(&g_outcome_warned_inactive, false);
    atomic_store(&g_outcome_warned_active, false);
    g_outcome_last_tick = 0;
}
#endif

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_daemon_proactive_outcome_record_send(hu_memory_t *memory, const char *channel,
                                                   const char *contact, size_t contact_len) {
    if (!memory || !channel || !contact || contact_len == 0)
        return HU_OK; /* best-effort: never block the send path */
    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_OK;
    /* message_ref is NULL: proactive sends have no per-message id at this
     * layer, and the resolver below matches by channel+contact alone. */
    return hu_dpo_collector_insert_proactive_send(db, channel, strlen(channel), contact,
                                                  contact_len, NULL, 0);
}

hu_error_t hu_daemon_proactive_outcome_mark_reply(hu_memory_t *memory, const char *channel,
                                                  const char *contact, size_t contact_len) {
    if (!memory || !channel || !contact || contact_len == 0)
        return HU_OK;
    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_OK;
    /* NULL message_ref = resolve every pending row for this contact: an
     * inbound message answers all outstanding proactives to them. */
    return hu_dpo_collector_update_proactive_outcome(db, channel, strlen(channel), contact,
                                                     contact_len, NULL, 0, HU_BANDIT_REPLY);
}

hu_error_t hu_daemon_proactive_outcome_tick(hu_memory_t *memory,
                                            struct hu_contextual_bandit *bandit, int64_t now) {
    if (g_outcome_last_tick != 0 &&
        now - g_outcome_last_tick < HU_PROACTIVE_OUTCOME_TICK_INTERVAL_S)
        return HU_OK;
    g_outcome_last_tick = now;

    sqlite3 *db = memory ? hu_sqlite_memory_get_db(memory) : NULL;
    if (!db || !bandit) {
        if (!atomic_exchange(&g_outcome_warned_inactive, true))
            hu_log_info("human", NULL,
                        "proactive-outcome learning tick inactive (%s) — bandit arms will not "
                        "move; sota init creates the bandit when SQLite memory is enabled",
                        !db ? "no sqlite db" : "sota.bandit not initialized");
        return HU_OK;
    }
    if (!atomic_exchange(&g_outcome_warned_active, true))
        hu_log_info("human", NULL,
                    "proactive-outcome learning tick active (%ds cadence): REPLY/IGNORED "
                    "outcomes now train the humanization bandit",
                    HU_PROACTIVE_OUTCOME_TICK_INTERVAL_S);
    return hu_proactive_outcomes_process_async(db, bandit);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_daemon_proactive_outcome_record_send(hu_memory_t *memory, const char *channel,
                                                   const char *contact, size_t contact_len) {
    (void)memory, (void)channel, (void)contact, (void)contact_len;
    return HU_OK;
}

hu_error_t hu_daemon_proactive_outcome_mark_reply(hu_memory_t *memory, const char *channel,
                                                  const char *contact, size_t contact_len) {
    (void)memory, (void)channel, (void)contact, (void)contact_len;
    return HU_OK;
}

hu_error_t hu_daemon_proactive_outcome_tick(hu_memory_t *memory,
                                            struct hu_contextual_bandit *bandit, int64_t now) {
    (void)memory, (void)bandit, (void)now;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

void hu_daemon_dpo_judge_tick(struct hu_agent *agent, struct hu_allocator *alloc, int64_t t) {
#ifdef HU_ENABLE_SQLITE
    /* DPO consolidation — train on preference pairs every 24 hours.
     *
     * BUGFIX 2026-05-25: Same first-tick blocking issue as the ML
     * experiment loop — judge_step makes 64 LLM scoring calls (32 pairs
     * × 2 each) for ~5 min synchronous blocking. That prevented the
     * iMessage channel dispatcher from running. Defer first run by
     * initializing last_dpo_train to current time. Subsequent runs
     * follow the 24-hour cadence. */
    static int64_t last_dpo_train = 0;
    int64_t dpo_interval = 24 * 3600;
    if (last_dpo_train == 0) {
        last_dpo_train = t;
        /* Skip first run; resume normal 24h cadence afterward. */
    } else if (agent && agent->memory && agent->sota.sota_initialized &&
               (t - last_dpo_train) >= dpo_interval) {
        sqlite3 *dpo_db = hu_sqlite_memory_get_db(agent->memory);
        if (dpo_db) {
            last_dpo_train = t;
            hu_dpo_judge_result_t dpo_result = {0};
            hu_error_t dpo_err =
                hu_dpo_judge_step(&agent->sota.dpo_collector, alloc, &agent->provider,
                                  agent->model_name, agent->model_name_len, 0.1, 32, &dpo_result);
            if (dpo_err == HU_OK && dpo_result.pairs_evaluated > 0)
                hu_log_info("human", agent ? agent->observer : NULL,
                            "DPO judge step: loss=%.4f, alignment=%.2f, "
                            "pairs=%zu",
                            dpo_result.loss, dpo_result.alignment_score,
                            dpo_result.pairs_evaluated);
        }
    }
#else
    (void)agent;
    (void)alloc;
    (void)t;
#endif
}
