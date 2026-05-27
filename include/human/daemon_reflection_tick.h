/* include/human/daemon_reflection_tick.h — T9 daemon adapter for the
 * M2 reflection loop.
 *
 * The reflection module (include/human/reflection.h, src/reflection)
 * is intentionally daemon-free — it takes an inputs_t struct, not a
 * hu_daemon pointer. This file bridges that boundary: the daemon
 * calls hu_daemon_tick_reflection_loop() every main-loop iteration,
 * we extract db/provider/allocator from the agent, build the inputs
 * struct, and invoke hu_reflection_run.
 *
 * Phase 1 ships with a STUB turn iterator that returns zero turns —
 * just enough for operators to opt in via config.json and verify the
 * wire-up without bootstrapping a real turn ledger. The production
 * iter wiring lives in a follow-up task (see T10 in
 * docs/plans/2026-05-26-reflection-loop/tasks.md). */

#ifndef HU_DAEMON_REFLECTION_TICK_H
#define HU_DAEMON_REFLECTION_TICK_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_config;
struct hu_agent;

/* Per-tick entry point. Cheap (returns immediately) when:
 *   - cfg->reflection_loop.enabled is false
 *   - agent is NULL (boot transient)
 *   - agent->memory is not SQLite-backed
 *   - hu_reflection_should_run gates the run (interval / not-idle)
 *
 * Otherwise, calls hu_reflection_run. Phase 1 stub iter returns 0
 * turns → status=NO_INPUT and a one-shot info log nudges the
 * operator that the production turn source isn't wired yet.
 *
 * Also runs hu_reflection_storage_migrate() exactly once across the
 * process lifetime (subsequent ticks no-op past the migration). */
void hu_daemon_tick_reflection_loop(const struct hu_config *cfg, struct hu_agent *agent,
                                    hu_allocator_t *alloc, uint64_t now_ms,
                                    uint64_t last_user_activity_ms);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_REFLECTION_TICK_H */
