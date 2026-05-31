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
 * Turn source (T9-followup, the resolved T4 decision): the production
 * iter streams the canonical `messages` conversation ledger — the same
 * table the memory engine's save_message path writes — via
 * hu_reflection_sqlite_turn_source (src/reflection/turn_source.c). It
 * reads only the sqlite3 handle, so the reflection module stays
 * daemon-free. NO_INPUT now means the ledger is genuinely empty (no
 * conversations yet), not "unwired". */

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
     * call when the operator hasn't opted in. This early-return is reached
     * BEFORE reflection.c's own disable-log, so without a one-shot line here
     * the daemon's reflection loop is silently off — the 2026-05-18 class of
     * invisible-dataloss bug (silent-config-gated-subsystems.md). */
    if (!cfg->reflection_loop.enabled) {
        static atomic_bool warned_disabled = false;
        hu_log_info_once(&warned_disabled, "reflection.daemon", NULL,
                         "reflection_loop subsystem disabled by config "
                         "(cfg->reflection_loop.enabled=false); set "
                         "reflection_loop.enabled=true in config.json to activate");
        return;
    }

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

    /* Production turn source: stream the most-recent slice of the
     * `messages` ledger, oldest-first. Cursor is prepared per tick and
     * disposed after the run — cheap (one prepared stmt) and keeps the
     * cursor's lifetime tightly bounded to the run. If the cursor can't
     * be prepared (e.g. the memory backend hasn't created `messages`
     * yet) warn once and skip this tick rather than crash. */
    hu_reflection_sqlite_turn_source_t *src = NULL;
    hu_error_t src_err = hu_reflection_sqlite_turn_source_init(
        &src, db, alloc, HU_REFLECTION_TURN_SOURCE_DEFAULT_MAX);
    if (src_err != HU_OK || !src) {
        static atomic_bool warned_no_source = false;
        hu_log_warn_once(&warned_no_source, "reflection.daemon", NULL,
                         "reflection turn source unavailable (err=%d) — the `messages` "
                         "ledger may not exist yet; skipping reflection this tick",
                         (int)src_err);
        return;
    }

    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg->reflection_loop,
        .provider = &agent->provider,
        .alloc = alloc,
        .iter_fn = hu_reflection_sqlite_turn_iter,
        .iter_ctx = src,
        .last_user_activity_ms = last_user_activity_ms,
        .now_ms = now_ms,
        .max_input_chars = 0, /* default 100K */
    };

    hu_reflection_run_status_t status = HU_REFLECTION_RUN_GATED;
    int kept = 0, dropped = 0;
    (void)hu_reflection_run(&inputs, /*force=*/false, &status, &kept, &dropped);

    hu_reflection_sqlite_turn_source_dispose(src);

    /* Per-tick logging: only emit something when a run actually
     * happened OR an error fired. Gated ticks are too noisy to log
     * each iteration. */
    switch (status) {
    case HU_REFLECTION_RUN_OK:
        hu_log_info("reflection.daemon", NULL, "reflection run complete: kept=%d dropped=%d", kept,
                    dropped);
        break;
    case HU_REFLECTION_RUN_NO_INPUT:
        /* The `messages` ledger is empty this tick — no conversations to
         * reflect on yet. One-shot to avoid log flooding on a fresh
         * install before any messages accrue. */
        {
            static atomic_bool warned_no_input = false;
            hu_log_info_once(&warned_no_input, "reflection.daemon", NULL,
                             "reflection_loop enabled but the conversation ledger is "
                             "empty (0 turns) — nothing to reflect on yet");
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

    /* T12 of docs/plans/2026-05-26-reflection-loop: failure-rate
     * watchdog. Fires at most once per process if > 50% of runs over
     * the last 24h failed. Cheap when the rate is healthy (single
     * SELECT COUNT) and idempotent past the first warning. */
    hu_reflection_check_failure_rate(db, now_ms, cfg->reflection_loop.enabled);
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
