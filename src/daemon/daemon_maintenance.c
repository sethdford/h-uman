/**
 * daemon_maintenance.c — Once-per-minute background maintenance ticks extracted
 * from the hu_service_run cron block in daemon.c (DDD Phase E2 daemon split,
 * chip 3 — see docs/plans/2026-05-29-ddd-bounded-contexts/phase-E2-daemon-service-lifecycle.md).
 *
 * Production-only, cron-build-only (compiled out under HU_IS_TEST and without
 * HU_HAS_CRON, mirroring the original inline blocks):
 *   - hu_daemon_maintenance_tick — verifier-metrics + prompt-budget flush,
 *     periodic memory consolidation, Phase 8 scheduled reflection engine
 *   - hu_daemon_learning_scheduler_tick — W14 scheduler tick, W13 outcome
 *     drain, LoRA auto-enqueue triggers, personal-model idle decay (SQLite)
 *
 * All bodies moved VERBATIM (behavior-preserving); function-static cadence
 * state moved with them (process lifetime is unchanged — hu_service_run is a
 * per-process singleton loop).
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "human/daemon_maintenance.h"
#include "human/agent.h"
#include "human/agent/prompt_budget.h"
#include "human/agent/scheduler.h"
#include "human/agent/training_runner_shared.h"
#include "human/agent/verifier_metrics.h"
#include "human/agent/world_model_bridge.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/intelligence/meta_learning.h"
#include "human/intelligence/reflection.h"
#include "human/intelligence/skills.h"
#include "human/memory.h"
#include "human/memory/consolidation.h"
#include "human/memory/consolidation_engine.h"
#include "human/memory/forgetting.h"
#include "human/memory/forgetting_curve.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#if defined(HU_HAS_CRON) && !defined(HU_IS_TEST)

void hu_daemon_maintenance_tick(hu_allocator_t *alloc, struct hu_agent *agent,
                                const hu_config_t *config, time_t t) {
    (void)t; /* only read by the HU_HAS_SKILLS reflection block */
    /* W4 verifier metrics flush — snapshot the per-process counters
     * onto disk so `human doctor verifier` (and any future
     * dashboard) can show the last known hallucination rate even
     * when the daemon is offline. 60s cadence is a heartbeat, not
     * a real-time stream; the file is small (~150B) and overwritten
     * in place, so cost is negligible. Skipped under HU_IS_TEST
     * because the test harness has its own ad-hoc HOME and the
     * shared metrics file would race across parallel tests. */
    if (agent) {
        static int64_t last_verifier_flush_ms = 0;
        struct timespec ts_vf;
        clock_gettime(CLOCK_MONOTONIC, &ts_vf);
        int64_t now_vf_ms = (int64_t)ts_vf.tv_sec * 1000 + ts_vf.tv_nsec / 1000000;
        if (last_verifier_flush_ms == 0)
            last_verifier_flush_ms = now_vf_ms;
        if (now_vf_ms - last_verifier_flush_ms >= 60000) {
            hu_verifier_metrics_t snap = {
                .total_runs = agent->verifier_runs,
                .total_claims_extracted = agent->verifier_claims_total,
                .total_claims_flagged = agent->verifier_claims_flagged,
                .last_update_epoch = 0, /* set by save() */
            };
            (void)hu_verifier_metrics_save(&snap);
            last_verifier_flush_ms = now_vf_ms;
        }
        /* prompt_budget snapshot flush — operator visibility for
         * the B3 Phase 1 accumulator. Mirrors verifier's 60s
         * cadence but uses the atomic Personal Model write
         * discipline (verifier's own write is non-atomic — a
         * documented weakness; we don't propagate it here).
         * See docs/plans/2026-05-25-doctor-prompt-budget-initiative/. */
        static int64_t last_pb_flush_ms = 0;
        if (last_pb_flush_ms == 0)
            last_pb_flush_ms = now_vf_ms;
        if (agent->prompt_budget && now_vf_ms - last_pb_flush_ms >= 60000) {
            (void)hu_prompt_budget_save_snapshot(agent->prompt_budget);
            last_pb_flush_ms = now_vf_ms;
        }
    }
    /* Periodic memory consolidation */
    if (config && config->consolidation_interval_hours > 0 && agent && agent->memory) {
        static int64_t last_consolidation_ms = 0;
        int64_t interval_ms = (int64_t)config->consolidation_interval_hours * 3600000LL;
        struct timespec ts_cons;
        clock_gettime(CLOCK_MONOTONIC, &ts_cons);
        int64_t now_ms = (int64_t)ts_cons.tv_sec * 1000 + ts_cons.tv_nsec / 1000000;
        if (last_consolidation_ms == 0)
            last_consolidation_ms = now_ms;
        if (now_ms - last_consolidation_ms >= interval_ms) {
            hu_consolidation_config_t cons_cfg = {
                .decay_days = config ? config->behavior.decay_days : 30,
                .decay_factor = 0.5,
                .dedup_threshold = config ? config->behavior.dedup_threshold : 0,
                .max_entries = 5000,
                .provider = &agent->provider,
                .model = agent->model_name,
                .model_len = agent->model_name_len,
            };
            if (hu_memory_consolidate(alloc, agent->memory, &cons_cfg) == HU_OK) {
                last_consolidation_ms = now_ms;
                hu_log_info("human", agent ? agent->observer : NULL,
                            "periodic memory consolidation completed");
            }
        }
    }
#ifdef HU_HAS_SKILLS
    /* Phase 8 (F77-F82): Scheduled reflection engine */
    {
        static bool reflection_done_today = false;
        static bool reflection_done_week = false;
        static bool reflection_done_month = false;
        struct tm tm_refl;
#if defined(_WIN32) && !defined(__CYGWIN__)
        struct tm *lt_refl = (localtime_s(&tm_refl, &t) == 0) ? &tm_refl : NULL;
#else
        struct tm *lt_refl = localtime_r(&t, &tm_refl);
#endif
        if (lt_refl) {
            /* Daily: 2-4 AM */
            if (lt_refl->tm_hour >= 2 && lt_refl->tm_hour < 4 && lt_refl->tm_min == 0 &&
                !reflection_done_today && agent && agent->memory) {
                sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
                if (refl_db) {
                    hu_reflection_engine_t refl_engine = {.alloc = alloc, .db = refl_db};
                    hu_reflection_daily(&refl_engine, (int64_t)t);
                    reflection_done_today = true;
                    if (agent->bth_metrics)
                        agent->bth_metrics->reflections_daily++;

                    /* P7: Nightly consolidation after daily reflection */
                    static int64_t last_consol_nightly = 0;
                    static int64_t last_consol_weekly = 0;
                    static int64_t last_consol_monthly = 0;
                    hu_consolidation_engine_t consol = {.alloc = alloc, .db = refl_db};
                    (void)hu_consolidation_engine_run_scheduled(
                        &consol, (int64_t)t, last_consol_nightly, last_consol_weekly,
                        last_consol_monthly);
                    last_consol_nightly = (int64_t)t;

                    /* P7: Forgetting curve batch decay */
                    (void)hu_forgetting_apply_batch_decay(refl_db, (int64_t)t, 0.1);

                    /* Vtable-based memory decay + prune */
                    if (agent && agent->memory) {
                        hu_forgetting_stats_t decay_stats = {0};
                        if (hu_memory_decay(alloc, agent->memory, 0.05, &decay_stats) == HU_OK &&
                            decay_stats.decayed > 0)
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "memory decay: %zu decayed", decay_stats.decayed);
                        hu_forgetting_stats_t prune_stats = {0};
                        if (hu_memory_prune(alloc, agent->memory, 0.01, &prune_stats) == HU_OK &&
                            prune_stats.pruned > 0)
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "memory prune: %zu pruned", prune_stats.pruned);
                    }

                    /* P7: Emotional residue decay (reduce intensity of old entries) */
                    /* Decay is applied on read via exponential formula; no separate
                     * batch call needed — hu_emotional_residue_get_active already
                     * applies intensity * exp(-decay_rate * days) on every retrieval.
                     */

                    /* P8: Refresh skill cache after reflection (new skills may exist)
                     */
                    {
                        hu_skill_t *refreshed = NULL;
                        size_t ref_count = 0;
                        if (hu_skill_load_active(alloc, refl_db, NULL, 0, &refreshed, &ref_count) ==
                                HU_OK &&
                            refreshed)
                            hu_skill_free(alloc, refreshed, ref_count);
                    }
                }
            }
            if (lt_refl->tm_hour == 5)
                reflection_done_today = false;

            /* Weekly: Sunday 3 AM */
            if (lt_refl->tm_wday == 0 && lt_refl->tm_hour == 3 && lt_refl->tm_min == 0 &&
                !reflection_done_week && agent && agent->memory) {
                sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
                if (refl_db) {
                    hu_reflection_engine_t refl_engine = {.alloc = alloc, .db = refl_db};
                    hu_reflection_weekly(&refl_engine, (int64_t)t);
                    reflection_done_week = true;
                    if (agent->bth_metrics)
                        agent->bth_metrics->reflections_weekly++;
                }
            }
            if (lt_refl->tm_wday == 1 && lt_refl->tm_hour == 0)
                reflection_done_week = false;

            /* Monthly: 1st 3 AM */
            if (lt_refl->tm_mday == 1 && lt_refl->tm_hour == 3 && lt_refl->tm_min == 0 &&
                !reflection_done_month && agent && agent->memory) {
                sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
                if (refl_db) {
                    hu_reflection_engine_t refl_engine = {.alloc = alloc, .db = refl_db};
                    hu_reflection_extract_general_lessons(&refl_engine, (int64_t)t);
                    hu_meta_params_t meta_params = {0};
                    hu_meta_learning_optimize(refl_db, &meta_params);
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "meta-learning: confidence=%.2f, refinement=%dw, "
                                "discovery_min=%d",
                                meta_params.default_confidence_threshold,
                                meta_params.refinement_frequency_weeks,
                                meta_params.discovery_min_feedback_count);
                    reflection_done_month = true;
                }
            }
            if (lt_refl->tm_mday == 2)
                reflection_done_month = false;
        }
    }
#endif
}

#ifdef HU_ENABLE_SQLITE
void hu_daemon_learning_scheduler_tick(struct hu_agent *agent, const hu_config_t *config,
                                       time_t t) {
    (void)config; /* only read by the HU_ENABLE_LEARNING trigger block */
    /* W14 sleep-time compute scheduler tick (FIX 13). The
     * scheduler is enqueue-driven: tick is a no-op when no
     * jobs are pending, and the per-tick total budget is
     * bounded internally (HU_SCHED_TOTAL_BUDGET_MS). We
     * rate-limit to once per minute so this stays cheap on
     * fast loops; the scheduler itself doesn't care how
     * often it's called. */
    if (agent && agent->w14_scheduler) {
        int64_t now_ms = (int64_t)t * 1000LL;
        if (agent->scheduler_last_tick_ms == 0 || now_ms - agent->scheduler_last_tick_ms >= 60000) {
            /* W13 outcome-bridge drain — must run BEFORE the
             * scheduler tick so any newly-emitted signals are
             * visible to the same training pass that the tick
             * may dispatch. The bridge is a no-op when learner
             * is NULL (ML build disabled) or when no new
             * outcomes have arrived since the last drain.
             *
             * Both this drain and the LoRA auto-enqueue below
             * call into the W13 learning loop — they're
             * compile-gated together with the runner-source
             * gating in CMakeLists.txt. When learning is off
             * `agent->learner` is also always NULL, so even
             * if a future caller forgot the gate the runtime
             * NULL check would prevent a crash. */
#if defined(HU_ENABLE_LEARNING)
            if (agent->learner && agent->outcomes) {
                hu_error_t be = hu_learner_bridge_emit_outcomes(agent->learner, agent->outcomes);
                if (be != HU_OK && be != HU_ERR_OUT_OF_MEMORY) {
                    /* OOM is the only expected failure (pending
                     * buffer full). Anything else is unexpected
                     * but not worth halting the scheduler over. */
                    hu_log_warn("human", agent->observer,
                                "w13 outcome-bridge drain failed (%s); continuing",
                                hu_error_string(be));
                }
            }
            /* W13/W14 — auto-enqueue LoRA training when enough
             * signals have accumulated. The runner is already
             * registered; we just need a job in the queue.
             *
             * Spec 2026-05-19 (Task 2): routes through the
             * shared training-runner entry alongside the new
             * pair-count trigger so both triggers produce
             * structurally identical scheduler records. */
            if (agent->learner) {
                size_t pending = hu_learner_pending_count(agent->learner);
                if (pending >= 10) {
                    (void)hu_training_runner_enqueue_lora_persona(
                        agent->w14_scheduler, now_ms, 300000, HU_TRAINING_TRIGGER_LEARNER_PENDING,
                        agent->observer);
                }
            }
            /* Spec 2026-05-19 (Task 3) — DPO pair-count trigger.
             * When uncommitted reaction-derived DPO pairs cross
             * `learning.dpo_pair_training_threshold`, enqueue a
             * training run via the same shared entry. Per
             * ~/.claude/rules/silent-config-gated-subsystems.md,
             * emit one info-level line on first tick for both
             * the disabled (threshold==0) and enabled paths. */
            if (agent && agent->sota.dpo_collector.alloc) {
                int threshold = config ? config->learning.dpo_pair_training_threshold
                                       : HU_LEARNING_DPO_PAIR_TRAINING_THRESHOLD_DEFAULT;
                static atomic_bool warned_pair_count_disabled = false;
                static atomic_bool warned_pair_count_enabled = false;
                if (threshold <= 0) {
                    hu_log_info_once(&warned_pair_count_disabled, "daemon", agent->observer,
                                     "DPO pair-count training trigger disabled by config "
                                     "(learning.dpo_pair_training_threshold=0); set "
                                     "learning.dpo_pair_training_threshold to a positive integer "
                                     "in config.json to activate");
                } else {
                    hu_log_info_once(&warned_pair_count_enabled, "daemon", agent->observer,
                                     "DPO pair-count training trigger active "
                                     "(learning.dpo_pair_training_threshold=%d)",
                                     threshold);
                    /* Spec 2026-05-19 M3 closure / AC-M3-7 —
                     * frontier-MLX auto-training gate. Emit one
                     * info-line on first tick whether the gate
                     * is on or off, per
                     * ~/.claude/rules/silent-config-gated-subsystems.md. */
                    bool frontier_auto =
                        config ? config->learning.m3_frontier_auto_training : false;
                    static atomic_bool warned_frontier_disabled = false;
                    static atomic_bool warned_frontier_enabled = false;
                    if (frontier_auto) {
                        hu_log_info_once(&warned_frontier_enabled, "daemon", agent->observer,
                                         "M3 frontier-MLX auto-training ENABLED "
                                         "(learning.m3_frontier_auto_training=true) — pair-count "
                                         "trigger will dispatch with target=frontier_mlx");
                    } else {
                        hu_log_info_once(
                            &warned_frontier_disabled, "daemon", agent->observer,
                            "M3 frontier-MLX auto-training disabled by config "
                            "(learning.m3_frontier_auto_training=false); pair-count "
                            "trigger will dispatch with target=huml_reference. Set "
                            "learning.m3_frontier_auto_training=true in config.json to "
                            "activate the M3 closure path");
                    }
                    size_t pair_count = 0;
                    if (hu_dpo_pair_count(&agent->sota.dpo_collector, &pair_count) == HU_OK &&
                        hu_training_runner_pair_count_should_fire(pair_count, threshold)) {
                        /* Refire cooldown: the counter is not consumed by a
                         * training run (pairs stay banked), so once it crosses
                         * the threshold should_fire is true on EVERY tick.
                         * Before the 2026-07-25 hydration fix this was masked
                         * by the counter resetting each restart; hydrated, an
                         * uncapped trigger would enqueue a 31B training run
                         * per maintenance tick. One dispatch per 24h. */
                        static int64_t last_pair_count_fire_ms = 0;
                        if (last_pair_count_fire_ms == 0 ||
                            now_ms - last_pair_count_fire_ms >= 86400000LL) {
                            last_pair_count_fire_ms = now_ms;
                            hu_training_target_model_t target =
                                frontier_auto ? HU_TRAINING_TARGET_FRONTIER_MLX
                                              : HU_TRAINING_TARGET_HUML_REFERENCE;
                            (void)hu_training_runner_enqueue_lora_persona_target(
                                agent->w14_scheduler, now_ms, 300000,
                                HU_TRAINING_TRIGGER_PAIR_COUNT, target, agent->observer);
                        }
                    }
                }
            }
#endif /* HU_ENABLE_LEARNING — outcome drain + LoRA auto-enqueue */
            /* Continuous learning loop: enqueue training data
             * extraction every 100 scheduler ticks (~100 min)
             * or every 6 hours, whichever comes first. The
             * runner extracts new conversations and auto-DPO
             * pairs, and enqueues LoRA retraining when enough
             * examples have accumulated. */
            {
                static int64_t last_td_extract_ms = 0;
                bool td_due = (agent->scheduler_ticks % 100 == 0) || (last_td_extract_ms == 0) ||
                              (now_ms - last_td_extract_ms >= 21600000LL);
                if (td_due) {
                    hu_error_t tde = hu_w14_scheduler_enqueue_training_data_extract(
                        agent->w14_scheduler, now_ms, 120000);
                    if (tde == HU_OK)
                        last_td_extract_ms = now_ms;
                }
            }
            /* US-7.5: nightly LoRA retrain enqueue. Fires once
             * per 24h. The scheduler enforces idle + AC-power
             * gating per the W14 contract; the runner's PID-file
             * prevents overlapping retrains across ticks. */
            {
                static int64_t last_retrain_enqueue_ms = 0;
                bool retrain_due = (last_retrain_enqueue_ms == 0) ||
                                   (now_ms - last_retrain_enqueue_ms >= 86400000LL);
                if (retrain_due) {
                    hu_error_t rre = hu_w14_scheduler_enqueue_lora_retrain_nightly(
                        agent->w14_scheduler, now_ms, 0);
                    if (rre == HU_OK)
                        last_retrain_enqueue_ms = now_ms;
                }
            }
            hu_error_t te = hu_w14_scheduler_tick(agent->w14_scheduler, now_ms);
            agent->scheduler_last_tick_ms = now_ms;
            agent->scheduler_ticks++;
            if (te != HU_OK) {
                hu_log_warn("human", agent->observer, "scheduler tick failed: %s",
                            hu_error_string(te));
            }
            /* Persist the post-tick snapshot for `human ml status`
             * and the doctor command. Best-effort: silent on
             * failure (matches the imessage poll-status pattern). */
            (void)hu_w14_scheduler_status_save(agent->w14_scheduler);

            /* Personal-model idle decay — covers the long-idle
             * daemon gap that the per-turn decay in agent_turn
             * can't fill. A daemon that goes hours / days
             * between user messages still ages signal in place
             * (decay state is purely derived from per-element
             * timestamps), but eager pruning makes room before
             * the next ingest's eviction policy sees a full
             * array of stale-but-high-raw-confidence facts.
             *
             * Rate-limited to once per hour via the
             * `hu_personal_model_idle_due` helper (extracted
             * for unit-testability — the whole daemon path
             * isn't reachable from the test binary, but the
             * is-it-time-yet predicate now is). apply_decay
             * is idempotent at fixed `now`, so calling more
             * often than the rate limit would be free
             * correctness-wise but wasteful (88 elements ×
             * float-multiply re-evaluated per tick). */
            {
                static int64_t last_pm_decay_secs = 0;
                const int64_t now_secs = (int64_t)t;
                if (hu_personal_model_idle_due(&last_pm_decay_secs, now_secs, 3600)) {
                    size_t pruned = hu_personal_model_apply_decay(&agent->personal_model, now_secs);
                    if (pruned > 0) {
                        hu_log_info("human", agent->observer,
                                    "personal model idle decay: pruned "
                                    "%zu stale entries",
                                    pruned);
                        /* Persist the pruned state — same
                         * crash-safety reasoning as the
                         * post-ingest save in agent_turn.
                         * Skip when the model has no content
                         * (first-run users). */
                        if (agent->auto_save &&
                            hu_personal_model_has_content(&agent->personal_model)) {
                            char pm_path[1024];
                            if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
                                (void)hu_personal_model_save(&agent->personal_model, pm_path);
                            }
                        }
                    }
                }
            }
        }
    }
}
#endif /* HU_ENABLE_SQLITE */

#endif /* HU_HAS_CRON && !HU_IS_TEST */
