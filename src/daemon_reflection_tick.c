/* src/daemon_reflection_tick.c — Daemon adapter for T5 reflection loop.
 *
 * T9 of docs/plans/2026-05-26-reflection-loop. Lives OUTSIDE the
 * reflection module proper (src/reflection/) because it imports
 * agent.h / memory.h to extract daemon state — keeping
 * src/reflection (the .c files) daemon-free as designed at T5.
 *
 * Three responsibilities:
 *
 *   1. Run hu_reflection_storage_migrate once at first tick. Gated
 *      by a static bool so subsequent ticks no-op past the migration.
 *
 *   2. Build hu_reflection_run_inputs_t from agent state on each tick
 *      (config, db, provider, allocator) + the current time.
 *
 *   3. Call hu_reflection_run() and log the verdict at info level.
 *      The internal gate decides whether to actually fire; this
 *      wrapper just feeds it the daemon's view of the world.
 *
 * Turn source (the deferred T4 decision): Phase 1 ships with a STUB
 * iter that always returns 0 turns. The hu_reflection_run gate
 * detects this and returns NO_INPUT, so the wire-up is verifiable
 * (operator can enable the subsystem in config, see startup logs +
 * tick logs, and confirm "no input source wired yet" without the
 * daemon crashing or producing nonsense patterns). A follow-up task
 * (tracked in tasks.md under T9-followup) wires the real turn
 * iterator once the team picks a turn source (see the four options
 * discussed at the top of this sprint's session log). */

#include "human/agent.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/memory.h"
#include "human/provider.h"
#include "human/reflection.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

/* ── Stub turn iter (T10 follow-up replaces this) ─────────────── */

/* Returns false on first call → hu_reflection_run sees 0 turns →
 * status=NO_INPUT, no provider call, no run row inserted. */
static bool stub_turn_iter(void *ctx, hu_reflection_turn_t *out_turn) {
    (void)ctx;
    (void)out_turn;
    return false;
}

/* ── One-shot migration ──────────────────────────────────────── */

static atomic_bool g_migrated = false;
static atomic_bool g_migration_warned = false;

static bool ensure_migrated(struct sqlite3 *db) {
    if (atomic_load(&g_migrated))
        return true;
    if (!db)
        return false;
    hu_error_t err = hu_reflection_storage_migrate(db);
    if (err != HU_OK) {
        hu_log_warn_once(&g_migration_warned, "reflection.daemon", NULL,
                         "reflection storage migration failed (err=%d) — subsystem will not "
                         "function this run; check db permissions and disk space",
                         (int)err);
        return false;
    }
    atomic_store(&g_migrated, true);
    return true;
}

/* ── Public tick wrapper ─────────────────────────────────────── */

/* Called from src/daemon.c main loop. Cheap when disabled in config:
 * the inner hu_reflection_should_run gate returns DISABLED and
 * hu_reflection_run is a no-op past the early-exit.
 *
 * `agent` may be NULL during very early daemon boot — we treat that
 * as "no work to do yet" rather than crashing.
 *
 * `cfg` is the root daemon config; we read cfg->reflection_loop. */
void hu_daemon_tick_reflection_loop(const hu_config_t *cfg, struct hu_agent *agent,
                                    hu_allocator_t *alloc, uint64_t now_ms,
                                    uint64_t last_user_activity_ms) {
    if (!cfg || !agent || !alloc)
        return;

    /* Cheap gate before we touch the db handle — saves us a sqlite
     * call when the operator hasn't opted in. */
    if (!cfg->reflection_loop.enabled)
        return;

    /* Reflection needs the same SQLite handle the rest of the memory
     * subsystem uses. If the memory backend isn't SQLite-backed (or
     * hasn't initialized yet), bail with a one-shot warn instead of
     * crashing. */
    if (!agent->memory)
        return;
    struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return;

    /* First tick after enabled-flip: migrate schema. */
    if (!ensure_migrated(db))
        return;

    /* Reflection uses the same provider the rest of the agent uses.
     * Future: route to a dedicated analytical-tier provider if cost
     * justifies (Gemini Flash for most ticks, Pro for a quarterly
     * deep-dive). For Phase 1 the agent's primary provider is fine. */
    if (!agent->provider.vtable || !agent->provider.vtable->chat_with_system)
        return;

    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg->reflection_loop,
        .provider = &agent->provider,
        .alloc = alloc,
        .iter_fn = stub_turn_iter,
        .iter_ctx = NULL,
        .last_user_activity_ms = last_user_activity_ms,
        .now_ms = now_ms,
        .max_input_chars = 0, /* default 100K */
    };

    hu_reflection_run_status_t status = HU_REFLECTION_RUN_GATED;
    int kept = 0, dropped = 0;
    (void)hu_reflection_run(&inputs, /*force=*/false, &status, &kept, &dropped);

    /* Per-tick logging: only emit something when a run actually
     * happened OR an error fired. Gated ticks are too noisy to log
     * each iteration. */
    switch (status) {
    case HU_REFLECTION_RUN_OK:
        hu_log_info("reflection.daemon", NULL, "reflection run complete: kept=%d dropped=%d", kept,
                    dropped);
        break;
    case HU_REFLECTION_RUN_NO_INPUT:
        /* This is the expected Phase 1 path — stub iter returns 0 turns.
         * One-shot to avoid log flooding when the operator enables the
         * subsystem before T10's real iter lands. */
        {
            static atomic_bool warned_no_input = false;
            hu_log_info_once(&warned_no_input, "reflection.daemon", NULL,
                             "reflection_loop enabled but no turn source wired "
                             "(stub iter returns 0 turns). T10 follow-up will "
                             "wire the production turn iterator.");
        }
        break;
    case HU_REFLECTION_RUN_PROVIDER_ERROR:
        hu_log_warn("reflection.daemon", NULL, "reflection provider call failed");
        break;
    case HU_REFLECTION_RUN_SCHEMA_INVALID:
        hu_log_warn("reflection.daemon", NULL, "reflection model returned malformed JSON");
        break;
    case HU_REFLECTION_RUN_STORAGE_ERROR:
        hu_log_warn("reflection.daemon", NULL, "reflection storage write failed");
        break;
    case HU_REFLECTION_RUN_GATED:
        /* Silent — the gate enum INSIDE should_run already explains
         * why (DISABLED / INTERVAL / NOT_IDLE), and emitting per-tick
         * would dominate the daemon log. */
        break;
    }
}

#else /* !HU_ENABLE_SQLITE */

void hu_daemon_tick_reflection_loop(const hu_config_t *cfg, struct hu_agent *agent,
                                    hu_allocator_t *alloc, uint64_t now_ms,
                                    uint64_t last_user_activity_ms) {
    (void)cfg;
    (void)agent;
    (void)alloc;
    (void)now_ms;
    (void)last_user_activity_ms;
}

#endif /* HU_ENABLE_SQLITE */
