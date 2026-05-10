#ifndef HU_AGENT_SCHEDULER_H
#define HU_AGENT_SCHEDULER_H

/* W14 — Sleep-Time Compute Scheduler.
 *
 * Single coordinator for v2's idle-compute jobs: wraps v1 AutoDream phases,
 * W10 KV-cache eviction / pre-warming, W11 belief re-verification, W13 LoRA
 * training, and the new counterfactual rehearsal runner.
 *
 * `hu_scheduler_t` owns a SQLite-backed job queue (`scheduler_jobs`) on the
 * graph's DB handle. `hu_scheduler_tick` dequeues jobs by (priority desc,
 * earliest_at asc), gates each on system-state probes (battery / quiet
 * hours), and dispatches to the registered `hu_job_runner_fn` for the
 * job's kind. Each runner is given an explicit per-job budget; the
 * scheduler also enforces a total per-tick budget so an OS-level fault in
 * any single runner cannot stall the daemon loop.
 *
 * Layer 4 of the v2 stack. The integration point in `src/daemon.c`'s 1 Hz
 * tick loop is intentionally NOT wired in this commit — see scheduler.c
 * for the documented hook.
 *
 * All OS-level probes (load, battery, AC power, quiet hours) honor
 * `HU_IS_TEST` and read deterministic values from
 *   HU_TEST_LOAD_PCT, HU_TEST_BATTERY_PCT, HU_TEST_ON_AC,
 *   HU_TEST_QUIET_HOURS
 * so test-suite runs never touch /proc, IOKit, or GetSystemPowerStatus. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls — scheduler does not need the full struct. */
struct hu_persona;
typedef struct hu_persona hu_persona_t;

typedef enum hu_job_kind {
    HU_JOB_AUTODREAM_QUARANTINE = 0,
    HU_JOB_AUTODREAM_COMMUNITY,
    HU_JOB_AUTODREAM_DECAY,
    HU_JOB_KV_CACHE_EVICTION,
    HU_JOB_KV_CACHE_WARMING,
    HU_JOB_LORA_TRAINING,
    HU_JOB_COUNTERFACTUAL_REHEARSAL,
    HU_JOB_BELIEF_REVERIFICATION,
    HU_JOB_KIND_MAX
} hu_job_kind_t;

/* Job specification.  `contact_id` may be NULL for global jobs.  Strings
 * are copied into the queue at enqueue time — the caller may free them
 * immediately after `hu_scheduler_enqueue` returns. */
typedef struct hu_job_spec {
    hu_job_kind_t kind;
    const char *contact_id;
    size_t contact_id_len;
    int priority;            /* 0 = normal, 1 = high (run before normal) */
    int budget_ms;           /* per-job wall budget; 0 = scheduler default (60 s) */
    int interval_sec;        /* repeat interval; 0 = run once */
    bool requires_idle;      /* skip if probe_load_pct > HU_SCHED_IDLE_LOAD_MAX */
    bool requires_ac_power;  /* skip if !on_ac_power */
    int64_t earliest_at;     /* unix ms; 0 = ASAP */
    int64_t latest_at;       /* unix ms; 0 = no deadline */
} hu_job_spec_t;

typedef struct hu_scheduler_status {
    int system_load_pct;       /* 0-100 */
    int battery_pct;           /* 0-100, -1 if unknown */
    bool on_ac_power;
    bool quiet_hours_active;
    size_t jobs_pending;
    size_t jobs_completed_today;
} hu_scheduler_status_t;

typedef struct hu_scheduler hu_scheduler_t;

/* Job runner: invoked by the scheduler with the originating spec and a
 * remaining wall budget.  Runners are expected to honor `budget_ms` —
 * the scheduler enforces a total per-tick budget but cannot interrupt a
 * runner already in C code.  Returning anything other than `HU_OK`
 * marks the job `failed` with the error string `hu_error_string(rc)`. */
typedef hu_error_t (*hu_job_runner_fn)(hu_memory_t *m, const hu_job_spec_t *spec,
                                       int64_t budget_ms, void *user_data);

/* Lifecycle.  `hu_scheduler_open` ensures the `scheduler_jobs` and
 * `counterfactual_replays` tables exist on the graph's SQLite handle and
 * registers a no-op runner for every `hu_job_kind_t`.  Tests and the
 * daemon overwrite individual runners with `hu_scheduler_register_runner`
 * before calling `hu_scheduler_tick`. */
hu_error_t hu_scheduler_open(hu_allocator_t *alloc, hu_memory_t *m,
                             hu_scheduler_t **out);
void hu_scheduler_close(hu_scheduler_t *s, hu_allocator_t *alloc);

/* Enqueue a job.  The spec is validated (kind in range, budget_ms
 * non-negative, latest_at >= earliest_at when both are non-zero).
 * Returns HU_ERR_INVALID_ARGUMENT on bad input. */
hu_error_t hu_scheduler_enqueue(hu_scheduler_t *s, const hu_job_spec_t *job);

/* Run one scheduling pass.  Dispatches up to `HU_SCHED_MAX_JOBS_PER_TICK`
 * eligible jobs in priority order, respecting the total per-tick budget
 * (`HU_SCHED_TOTAL_BUDGET_MS`).  `now_ms` is used as the wall-clock
 * reference so tests can pin time. */
hu_error_t hu_scheduler_tick(hu_scheduler_t *s, int64_t now_ms);

/* Snapshot scheduler + system state.  Always populates `out` even on
 * partial probe failure. */
hu_error_t hu_scheduler_status(hu_scheduler_t *s, hu_scheduler_status_t *out);

/* Override the runner for `kind`.  The scheduler keeps a single
 * function pointer per kind — repeated registrations replace the prior
 * binding.  `fn == NULL` resets to the no-op default. `user_data` is
 * forwarded verbatim to the runner. */
hu_error_t hu_scheduler_register_runner(hu_scheduler_t *s, hu_job_kind_t kind,
                                        hu_job_runner_fn fn, void *user_data);

/* ── System-state probes (HU_IS_TEST aware) ──────────────────────────── */

/* Returns 0–100, or -1 if the host can't report load. */
int hu_scheduler_probe_load_pct(void);

/* Returns 0–100, or -1 if the host has no battery / can't report. */
int hu_scheduler_probe_battery_pct(void);

/* True if the host is on AC power; on hosts that can't tell, returns
 * true (favor running jobs over silent skip). */
bool hu_scheduler_probe_on_ac_power(void);

/* True if `now_ms` falls inside the persona's quiet-hours window.  Under
 * `HU_IS_TEST` honors `HU_TEST_QUIET_HOURS`; production wiring is a
 * follow-up (the persona currently exposes time-of-day overlays but not
 * a structured quiet-hours window). */
bool hu_scheduler_probe_quiet_hours(int64_t now_ms, const hu_persona_t *persona);

/* ── Built-in runners ────────────────────────────────────────────────── */

/* Counterfactual rehearsal: for each recent reasoning trace from W10
 * with outcome='ok', writes a deterministic placeholder diff into the
 * `counterfactual_replays` table for any `relations` row that arrived
 * after the trace.  Caps at 5 rehearsals per tick.  Schema-creates
 * `counterfactual_replays` on first call.  Register via
 * `hu_scheduler_register_runner(s, HU_JOB_COUNTERFACTUAL_REHEARSAL,
 *                                hu_counterfactual_rehearsal_runner,
 *                                NULL)`. */
hu_error_t hu_counterfactual_rehearsal_runner(hu_memory_t *m,
                                              const hu_job_spec_t *spec,
                                              int64_t budget_ms,
                                              void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_SCHEDULER_H */
