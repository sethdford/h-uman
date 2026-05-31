---
title: "W14 — Sleep-Time Compute Scheduler: hosts learning, KV-warming, counterfactual rehearsal"
created: 2026-05-10
status: closed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: medium
scope: include/human/agent/, src/agent/, src/daemon.c, src/ml/
last_audit: 2026-05-25
---

# W14 — Sleep-Time Compute Scheduler

## Goal

Extend v1's AutoDream from heuristic prune/summarize/decay into a full **idle-compute scheduler** (`hu_scheduler_t`). Hosts: W13 LoRA training jobs, W10 KV-cache pre-warming for likely-asked queries, counterfactual rehearsal ("if I had known X earlier, what would I have said?"), W11 self-RAG re-verification of stale beliefs. Runs around system load, battery state, user-defined quiet hours.

## Motivation

v1 AutoDream is a single-purpose function (review quarantine, summarize communities, decay edges). v2 adds:
- W10 KV-cache eviction + pre-warming
- W13 LoRA training jobs
- Counterfactual rehearsal
- Stale belief re-verification (W11 self-RAG over old facts)

If each lives as its own daemon hook, we get four cron-shaped jobs racing on the same DB. The right shape is a single coordinator.

## Prior art

- "Sleep-time compute" literature (2025): pre-derive answers, build query-specific subgraphs.
- v1 AutoDream — gets folded in as one job kind.
- `src/daemon.c` periodic loop — keeps running as host.

## Design

### Vtable

```c
/* include/human/agent/scheduler.h */

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

typedef struct hu_job_spec {
    hu_job_kind_t kind;
    const char *contact_id;
    size_t contact_id_len;
    int priority;          /* 0 = normal, 1 = high (run before normal) */
    int budget_ms;
    int interval_sec;      /* repeat interval; 0 = run once */
    bool requires_idle;    /* only run when system is idle */
    bool requires_ac_power;/* skip on battery */
    int64_t earliest_at;   /* unix ms; 0 = ASAP */
    int64_t latest_at;     /* unix ms; 0 = no deadline */
} hu_job_spec_t;

typedef struct hu_scheduler_status {
    int system_load_pct;     /* 0-100 */
    int battery_pct;         /* 0-100, -1 if unknown */
    bool on_ac_power;
    bool quiet_hours_active;
    size_t jobs_pending;
    size_t jobs_completed_today;
} hu_scheduler_status_t;

typedef struct hu_scheduler hu_scheduler_t;

hu_error_t hu_scheduler_open(hu_allocator_t *alloc, hu_memory_t *m, hu_scheduler_t **out);
hu_error_t hu_scheduler_enqueue(hu_scheduler_t *s, const hu_job_spec_t *job);
hu_error_t hu_scheduler_tick(hu_scheduler_t *s, int64_t now_ms);
hu_error_t hu_scheduler_status(hu_scheduler_t *s, hu_scheduler_status_t *out);
void hu_scheduler_close(hu_scheduler_t *s, hu_allocator_t *alloc);

/* Job runners — each has its own implementation; scheduler dispatches. */
typedef hu_error_t (*hu_job_runner_fn)(hu_memory_t *m, const hu_job_spec_t *spec,
                                       int64_t budget_ms);
hu_error_t hu_scheduler_register_runner(hu_scheduler_t *s, hu_job_kind_t kind,
                                        hu_job_runner_fn fn);
```

### Schema

```sql
CREATE TABLE IF NOT EXISTS scheduler_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind INTEGER NOT NULL,
    contact_id TEXT NOT NULL DEFAULT '',
    priority INTEGER NOT NULL DEFAULT 0,
    budget_ms INTEGER NOT NULL,
    interval_sec INTEGER NOT NULL DEFAULT 0,
    earliest_at INTEGER NOT NULL DEFAULT 0,
    latest_at INTEGER NOT NULL DEFAULT 0,
    requires_idle INTEGER NOT NULL DEFAULT 1,
    requires_ac_power INTEGER NOT NULL DEFAULT 0,
    last_run_at INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'pending',  /* pending, running, done, failed, expired */
    last_error TEXT
);
```

### System-state probes

```c
/* src/agent/scheduler_probes.c */
int hu_scheduler_probe_load_pct(void);          /* 0-100 */
int hu_scheduler_probe_battery_pct(void);       /* -1 if unknown */
bool hu_scheduler_probe_on_ac_power(void);
bool hu_scheduler_probe_quiet_hours(int64_t now_ms, const hu_persona_t *persona);
```

macOS: `IOKit` for battery. Linux: `/sys/class/power_supply`. Windows: `GetSystemPowerStatus`. All gated by `HU_IS_TEST` so tests get deterministic readings.

### Job runners (registered at scheduler_open)

- `HU_JOB_AUTODREAM_*` → existing `src/agent/simulation/autodream.c` functions.
- `HU_JOB_KV_CACHE_*` → W10 module.
- `HU_JOB_LORA_TRAINING` → W13 `hu_learner_train`.
- `HU_JOB_COUNTERFACTUAL_REHEARSAL` → new in `src/agent/simulation/counterfactual.c`.
- `HU_JOB_BELIEF_REVERIFICATION` → W11 `hu_self_rag_verify` over stale beliefs.

### Counterfactual rehearsal (new module)

For each recent reasoning trace (W10) that ended in `outcome = "ok"`, ask: "if I had known X (a fact added since the trace) earlier, would I have responded differently?" Record the diff in `counterfactual_replays` table for later use.

```sql
CREATE TABLE IF NOT EXISTS counterfactual_replays (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    trace_id INTEGER NOT NULL REFERENCES neural_reasoning_traces(id),
    new_fact_relation_id INTEGER NOT NULL REFERENCES relations(id),
    diff_text TEXT NOT NULL,
    confidence_mean REAL NOT NULL DEFAULT 0.5,
    rehearsed_at INTEGER NOT NULL
);
```

## Phases

1. `scheduler.h` + queue + tick loop.
2. System-state probes per platform.
3. Migrate v1 autodream into job-runner shape.
4. Register W10/W13 runners.
5. Author counterfactual rehearsal runner.
6. Daemon hook: tick once per minute.
7. Adversarial tests.

## Test plan

- `test_w14_scheduler_respects_priority`.
- `test_w14_scheduler_skips_jobs_on_battery_when_required`.
- `test_w14_scheduler_respects_quiet_hours`.
- `test_w14_scheduler_budget_enforced_per_job`.
- `test_w14_autodream_runs_via_scheduler_matches_v1`.
- `test_w14_lora_training_runs_when_signals_available`.
- `test_w14_counterfactual_rehearsal_records_diff`.
- `test_w14_adversarial_job_flood_respects_total_budget`.
- `test_w14_adversarial_runner_oom_does_not_crash_scheduler`.

## Success metric

- Idle-CPU usage during scheduled hours within 30% budget.
- LoRA training visible in `human ml status` log.
- KV-cache hit-rate improvement after warming run: +15 pts.
- Binary size delta ≤ +70 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Job runners block the scheduler | Each job runs in a thread with budget-enforced timeout |
| Battery-probe stub flakes in CI | `HU_IS_TEST` returns deterministic values |
| Counterfactual rehearsal generates infinite work | Per-tick cap on rehearsals (max 5) |

## Out of scope

- Distributed scheduler across multiple devices.
- ML-based job priority learning.

## Binary size budget: +70 KB.
