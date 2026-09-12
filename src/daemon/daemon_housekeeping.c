/* src/daemon/daemon_housekeeping.c — see include/human/daemon/housekeeping.h.
 * Carved out of hu_service_run (daemon.c) 2026-09-12, behavior-preserving. The
 * three loop variables the block advances are reached through ctx pointers;
 * everything else is a local alias of a read-only context field. */

#include "human/agent.h"
#include "human/agent/autodream.h"
#include "human/agent/training_runner_shared.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/rand.h"
#include "human/daemon.h"
#include "human/daemon/common.h"
#include "human/daemon/housekeeping.h"
#include "human/daemon/intelligence_facade.h"
#include "human/daemon/ml_facade.h"
#include "human/daemon/persona_facade.h"
#include "human/daemon/platform_facade.h"
#include "human/daemon/reactive_turn.h"
#include "human/daemon_cron.h"
#include "human/daemon_learning_tick.h"
#include "human/daemon_maintenance.h"
#include "human/memory/graph.h"
#include "human/ml/learner_bridge.h"
#include "human/ml/lora_nightly.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hu_daemon_housekeeping_tick(hu_daemon_housekeeping_ctx_t *ctx) {
#ifdef HU_HAS_CRON
    hu_allocator_t *alloc = ctx->alloc;
    hu_agent_t *agent = ctx->agent;
    const hu_config_t *config = ctx->config;
    hu_service_channel_t *channels = ctx->channels;
    size_t channel_count = ctx->channel_count;
    hu_graph_t *graph = ctx->graph;
    time_t t = ctx->t;
    time_t current_minute = ctx->current_minute;
    char *community_insights = ctx->community_insights;

    if (current_minute > (*ctx->last_cron_minute)) {
        hu_daemon_cron_tick(alloc);
        hu_service_run_agent_cron(alloc, agent, channels, channel_count);
        /* Run proactive check-ins at the top of each hour.
         * Add jitter (0-30 min) via deferred scheduling to avoid
         * blocking the daemon loop. */
        if (current_minute % 60 == 0 && (*ctx->proactive_due_at) == 0) {
            unsigned int jitter_sec = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
            jitter_sec = (unsigned int)hu_rand_uniform(1801);
#else
            {
                uint32_t s = (uint32_t)time(NULL);
                s = s * 1103515245u + 12345u;
                jitter_sec = (unsigned int)((s >> 16u) & 0x7fffu) % 1801u;
            }
#endif
            (*ctx->proactive_due_at) = t + (time_t)jitter_sec;
        }
        if ((*ctx->proactive_due_at) > 0 && t >= (*ctx->proactive_due_at)) {
            (*ctx->proactive_due_at) = 0;
#ifndef HU_IS_TEST
            /* Defined only outside HU_IS_TEST (daemon.c); in the original service loop
             * this whole tick sat in the non-test arm of the loop, so tests never
             * compiled the call. Kept that way for the one symbol that needs it. */
            hu_service_run_proactive_checkins(alloc, agent, channels, channel_count, config);
#endif
            if (agent && agent->bth_metrics)
                hu_bth_metrics_log(agent->bth_metrics);
        }
#ifndef HU_IS_TEST
        /* W4 verifier-metrics + prompt-budget flush, periodic memory
         * consolidation, and the Phase 8 scheduled reflection engine
         * moved to src/daemon/daemon_maintenance.c (E2 chip 3). */
        hu_daemon_maintenance_tick(alloc, agent, config, t);
#ifdef HU_ENABLE_SQLITE
        /* W14 scheduler tick, W13 outcome drain, LoRA auto-enqueue
         * triggers, and personal-model idle decay moved to
         * src/daemon/daemon_maintenance.c (E2 chip 3). */
        hu_daemon_learning_scheduler_tick(agent, config, t);
        /* A3 intrinsic motivation: bounded, default-OFF curiosity loop.
         * On a 5-min idle cadence, age the drive (rises while quiet) and
         * let the already-built, fully-gated runner decide whether to
         * originate an INTERNAL, propose-only goal. The runner has no
         * action surface, is hard-bounded (per-tick budget), preemptible
         * (user_active⇒no), audited, and disabled by default (emits a
         * one-shot disabled log naming the config key). Sharing (T5)
         * routes through init_proposer in a later layer behind this same
         * gate. Spec: docs/plans/2026-05-29-intrinsic-motivation/. */
        if (agent) {
            static int64_t last_intrinsic_tick_secs = 0;
            const int64_t now_secs = (int64_t)t;
            if (now_secs - last_intrinsic_tick_secs >= 300) {
                last_intrinsic_tick_secs = now_secs;
                hu_intrinsic_drive_tick(&agent->intrinsic_drive, false, now_secs);
                hu_intrinsic_runtime_cfg_t icfg = {
                    .enabled = config && config->intrinsic.enabled,
                    .per_tick_token_budget = config ? config->intrinsic.per_tick_token_budget : 0,
                };
                uint32_t tick_budget = icfg.per_tick_token_budget
                                           ? icfg.per_tick_token_budget
                                           : HU_INTRINSIC_DEFAULT_TICK_BUDGET;
                hu_intrinsic_start_facts_t ifacts = {
                    .drive_level = hu_intrinsic_drive_level(&agent->intrinsic_drive),
                    .secs_since_user = now_secs - agent->intrinsic_drive.last_user_ts,
                    .secs_since_intrinsic = now_secs - agent->intrinsic_drive.last_intrinsic_ts,
                    .budget_tokens_remaining = tick_budget,
                    .user_active = false,
                };
                hu_intrinsic_tick_result_t ires;
                hu_intrinsic_run_tick(&agent->intrinsic_drive, &icfg, &ifacts, agent->observer,
                                      now_secs, &ires);
                /* STARTED originates an internal goal only; run_tick emits
                 * the audit line. No egress here (propose-only is a later
                 * layer through init_proposer). */
            }
        }

        /* W2 AutoDream + W5 persona evolver — daily housekeeping.
         * Both prefer the W14 scheduler (paced, idle-gated) with
         * sync fallback for graceful degradation. Runs once per
         * day in the 3-5 AM window to avoid the 2 AM reflection
         * cycle's SQLite write locks. */
        {
            static bool autodream_done_today = false;
            static bool evolver_done_today = false;
            struct tm tm_dream;
#if defined(_WIN32) && !defined(__CYGWIN__)
            struct tm *lt_dream = (localtime_s(&tm_dream, &t) == 0) ? &tm_dream : NULL;
#else
            struct tm *lt_dream = localtime_r(&t, &tm_dream);
#endif
            if (lt_dream && graph) {
                if (lt_dream->tm_hour == 3 && lt_dream->tm_min == 0 && !autodream_done_today) {
                    /* Prefer the W14 scheduler path: enqueue the
                     * three AutoDream phases as separate jobs so
                     * they run paced + budgeted + idle/battery-
                     * gated, instead of blocking the 1 Hz daemon
                     * loop while quarantine review walks the
                     * graph. The scheduler's default runners
                     * already invoke hu_autodream_run with the
                     * right enable_* flags per phase
                     * (src/agent/scheduler.c
                     * default_autodream_*_runner). When the
                     * scheduler is unavailable (e.g. test mode,
                     * or W7 facade failed to open), fall back to
                     * the synchronous direct call so AutoDream
                     * still runs every night. */
                    if (agent && agent->w14_scheduler) {
                        hu_error_t enq_err = hu_w14_scheduler_enqueue_autodream(
                            agent->w14_scheduler, (int64_t)t * 1000LL,
                            60000 /* 1 min budget per phase */);
                        if (enq_err == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "autodream: enqueued 3 phases via W14 scheduler");
                        } else {
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "autodream W14 enqueue failed (%s) — falling "
                                         "back to direct sync run",
                                         hu_error_string(enq_err));
                            hu_autodream_config_t ad_cfg = hu_autodream_default_config();
                            ad_cfg.now_ms = (int64_t)t * 1000LL;
                            hu_autodream_report_t ad_report;
                            memset(&ad_report, 0, sizeof(ad_report));
                            hu_memory_facade_t *ad_m =
                                (agent && agent->w7_facade)
                                    ? hu_w7_facade_memory_handle(agent->w7_facade)
                                    : NULL;
                            if (ad_m)
                                (void)hu_autodream_run_on_facade(alloc, ad_m, &ad_cfg, &ad_report);
                            else
                                (void)hu_autodream_run(alloc, graph, &ad_cfg, &ad_report);
                        }
                    } else {
                        hu_autodream_config_t ad_cfg = hu_autodream_default_config();
                        ad_cfg.now_ms = (int64_t)t * 1000LL;
                        hu_autodream_report_t ad_report;
                        memset(&ad_report, 0, sizeof(ad_report));
                        hu_memory_facade_t *ad_m =
                            (agent && agent->w7_facade)
                                ? hu_w7_facade_memory_handle(agent->w7_facade)
                                : NULL;
                        hu_error_t ad_err =
                            ad_m ? hu_autodream_run_on_facade(alloc, ad_m, &ad_cfg, &ad_report)
                                 : hu_autodream_run(alloc, graph, &ad_cfg, &ad_report);
                        if (ad_err == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "autodream (sync): quarantine reviewed=%zu "
                                        "released=%zu dropped=%zu communities=%zu "
                                        "edges=%zu derived=%zu budget_exceeded=%d",
                                        ad_report.quarantine_reviewed,
                                        ad_report.quarantine_released, ad_report.quarantine_dropped,
                                        ad_report.communities_summarized,
                                        ad_report.edges_reweighted, ad_report.derived_facts_added,
                                        ad_report.budget_exceeded ? 1 : 0);
                        } else {
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "autodream failed: %s (last_error=%s)",
                                         hu_error_string(ad_err), ad_report.last_error);
                        }
                    }
                    autodream_done_today = true;
                }
                /* Persona evolver runs at 3:05 AM so it sees the
                 * fresh AutoDream output. Empty contact_id means
                 * "process global deltas"; per-contact evolution
                 * happens lazily on each turn (W5 future work). */
                if (lt_dream->tm_hour == 3 && lt_dream->tm_min == 5 && !evolver_done_today) {
                    if (agent && agent->w14_scheduler) {
                        hu_error_t enq_err = hu_w14_scheduler_enqueue_persona_evolver(
                            agent->w14_scheduler, (int64_t)t * 1000LL, 120000);
                        if (enq_err == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "persona evolver: enqueued via W14 scheduler");
                        } else {
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "persona evolver W14 enqueue failed (%s) "
                                         "— falling back to sync",
                                         hu_error_string(enq_err));
                            goto persona_evolver_sync;
                        }
                    } else {
                    persona_evolver_sync:;
                        hu_persona_evolver_config_t pe_cfg = hu_persona_evolver_default_config();
                        pe_cfg.now_ms = (int64_t)t * 1000LL;
                        hu_persona_evolver_report_t pe_report;
                        memset(&pe_report, 0, sizeof(pe_report));
                        hu_memory_facade_t *pe_m =
                            (agent && agent->w7_facade)
                                ? hu_w7_facade_memory_handle(agent->w7_facade)
                                : NULL;
                        hu_error_t pe_err =
                            pe_m ? hu_persona_evolver_run_facade(pe_m, "", 0, &pe_cfg, &pe_report)
                                 : hu_persona_evolver_run(graph, "", 0, &pe_cfg, &pe_report);
                        if (pe_err == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "persona evolver (sync): proposed=%zu applied=%zu "
                                        "dropped=%zu quarantined=%zu pending=%zu",
                                        pe_report.proposed_total, pe_report.applied,
                                        pe_report.dropped, pe_report.quarantined,
                                        pe_report.still_pending);
                        } else {
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "persona evolver failed: %s", hu_error_string(pe_err));
                        }
                    }
                    evolver_done_today = true;
                }
                if (lt_dream->tm_hour == 6) {
                    autodream_done_today = false;
                    evolver_done_today = false;
                }
            }
        }

        /* Sprint B N2 — LoRA nightly tick.
         *
         * Fires at 04:00 local time once per day, AFTER the 03:00
         * autodream + 03:05 persona evolver have finished their
         * SQLite work. Calls hu_lora_nightly_run, which:
         *   1. checks should_run (≥24h since last run + ≥20 new pairs)
         *   2. exports dpo_pairs → JSONL
         *   3. (when enabled) invokes mlx_lm.lora via subprocess
         *      with a 30-min hard timeout
         *   4. atomically rotates ~/.human/adapter-current
         *   5. POSTs /v1/adapters/swap to the live MLX server
         *
         * BLOCKING CAVEAT: subprocess training holds this loop
         * for up to 30 min. The 4 AM slot is chosen to minimize
         * user impact. Opt-in via `cfg.learning.nightly_lora_enabled`
         * (preferred) or env var HU_NIGHTLY_LORA_ENABLED=1
         * (legacy fallback, deprecated; one-shot warn fires when
         * the env path is used to nudge operators toward config).
         *
         * Last-run tracking is in-memory only for this slice —
         * after a daemon restart we'll re-run the next 4 AM
         * regardless of when the previous run was. Persisting to
         * ~/.human/lora-nightly.state is a clean follow-up. */
        {
            static bool lora_nightly_done_today = false;
            static int64_t lora_last_run_unix = 0;
            struct tm tm_nightly;
#if defined(_WIN32) && !defined(__CYGWIN__)
            struct tm *lt_nightly = (localtime_s(&tm_nightly, &t) == 0) ? &tm_nightly : NULL;
#else
            struct tm *lt_nightly = localtime_r(&t, &tm_nightly);
#endif
            /* M3 trivia closure (2026-05-26) — config-first gate
             * with env-var legacy fallback. Config wins; env is
             * only consulted when config didn't enable. */
            bool gate_on = config && config->learning.nightly_lora_enabled;
            if (!gate_on) {
                const char *nightly_enabled = getenv("HU_NIGHTLY_LORA_ENABLED");
                if (nightly_enabled && nightly_enabled[0] == '1') {
                    gate_on = true;
                    static atomic_bool warned_env_legacy = false;
                    hu_log_info_once(&warned_env_legacy, "lora-nightly",
                                     agent ? agent->observer : NULL,
                                     "HU_NIGHTLY_LORA_ENABLED env var is deprecated; "
                                     "set learning.nightly_lora_enabled=true in "
                                     "config.json instead. Env path still honored for "
                                     "backwards compatibility.");
                } else {
                    static atomic_bool warned_disabled = false;
                    hu_log_info_once(&warned_disabled, "lora-nightly",
                                     agent ? agent->observer : NULL,
                                     "nightly LoRA training disabled by config "
                                     "(cfg->learning.nightly_lora_enabled=false); set "
                                     "learning.nightly_lora_enabled=true in config.json "
                                     "to activate");
                }
            }
            if (lt_nightly && gate_on) {
                if (lt_nightly->tm_hour == 4 && lt_nightly->tm_min == 0 &&
                    !lora_nightly_done_today) {
                    hu_lora_nightly_config_t lcfg;
                    if (hu_lora_nightly_config_init_defaults(&lcfg)) {
                        /* Count new DPO pairs since last run (predicate gate). */
                        int32_t new_pairs_since = 0;
                        sqlite3 *db =
                            agent && agent->memory ? hu_sqlite_memory_get_db(agent->memory) : NULL;
                        if (db) {
                            /* AC-105.2 / AC-102.7: mine fresh production_outcomes into
                             * dpo_pairs BEFORE counting + training, so the nightly run
                             * trains on the latest implicit feedback. This closes the
                             * learning loop in production: outbound outcome → implicit
                             * signal → mining → dpo_pair → nightly train → hot-swap. */
                            int mined_pairs = 0;
                            hu_error_t mine_err = hu_dpo_collector_mine_pairs_from_outcomes(
                                db, INT_MAX, &mined_pairs);
                            if (mine_err == HU_OK && mined_pairs > 0) {
                                hu_log_info("lora-nightly", agent ? agent->observer : NULL,
                                            "mined %d DPO pair(s) from production_outcomes",
                                            mined_pairs);
                            }

                            const char *count_sql = "SELECT COUNT(*) FROM dpo_pairs";
                            sqlite3_stmt *stmt = NULL;
                            if (sqlite3_prepare_v2(db, count_sql, -1, &stmt, NULL) == SQLITE_OK) {
                                if (sqlite3_step(stmt) == SQLITE_ROW) {
                                    new_pairs_since = (int32_t)sqlite3_column_int(stmt, 0);
                                }
                                sqlite3_finalize(stmt);
                            }
                        }
                        /* Check if we should run: ≥20 pairs AND (never run OR ≥24h since
                         * last). */
                        if (hu_lora_nightly_should_run((int64_t)t, lora_last_run_unix,
                                                       new_pairs_since)) {
                            size_t pair_count = 0;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "lora-nightly: starting at 04:00 (may block up to "
                                        "30m)");
                            hu_error_t lr =
                                hu_lora_nightly_run(alloc, &lcfg, (int64_t)t, &pair_count);
                            if (lr == HU_OK) {
                                lora_last_run_unix = (int64_t)t;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "lora-nightly: ok (%zu pairs exported)", pair_count);
                            } else if (lr == HU_ERR_NOT_FOUND) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "lora-nightly: skipped (no new pairs)");
                            } else {
                                hu_log_warn("human", agent ? agent->observer : NULL,
                                            "lora-nightly: failed (err=%d)", (int)lr);
                            }
                        } else {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "lora-nightly: skipped predicate (pairs=%d, "
                                        "last_run=%lld, now=%lld)",
                                        (int)new_pairs_since, (long long)lora_last_run_unix,
                                        (long long)t);
                        }
                    }
                    lora_nightly_done_today = true;
                    (void)lora_last_run_unix; /* reserved for next-tier persistence */
                }
                if (lt_nightly->tm_hour == 6)
                    lora_nightly_done_today = false;
            }
        }
        /* P7: Feed processor poll — every 5 minutes (per-type intervals apply) */
        {
            static uint64_t last_feed_poll_types[HU_FEED_COUNT] = {0};
            static uint64_t last_feed_poll_global = 0;
            uint64_t fp_now = (uint64_t)t * 1000ULL;
            if (agent && agent->memory &&
                (last_feed_poll_global == 0 || (fp_now - last_feed_poll_global) >= 300000ULL)) {
                sqlite3 *fdb = hu_sqlite_memory_get_db(agent->memory);
                if (fdb) {
                    hu_feed_processor_t fp = {.alloc = alloc, .db = fdb};
                    if (config && config->feeds.interests) {
                        fp.interests = config->feeds.interests;
                        fp.interests_len = strlen(config->feeds.interests);
                        fp.relevance_threshold = config->feeds.relevance_threshold;
                    }
                    if (config) {
/* Borrow a config credential string + length into the processor. */
#define HU_FEED_CRED(field)                               \
    do {                                                  \
        if (config->feeds.field) {                        \
            fp.field = config->feeds.field;               \
            fp.field##_len = strlen(config->feeds.field); \
        }                                                 \
    } while (0)
                        HU_FEED_CRED(gmail_client_id);
                        HU_FEED_CRED(gmail_client_secret);
                        HU_FEED_CRED(gmail_refresh_token);
                        HU_FEED_CRED(gmail_quota_project);
                        HU_FEED_CRED(twitter_bearer_token);
#undef HU_FEED_CRED
                    }
                    hu_feed_config_t fconf;
                    memset(&fconf, 0, sizeof(fconf));
                    fconf.enabled[HU_FEED_NEWS_RSS] = true;
                    fconf.enabled[HU_FEED_FILE_INGEST] = true;
                    fconf.enabled[HU_FEED_GMAIL] = true;
                    fconf.enabled[HU_FEED_IMESSAGE] = true;
                    fconf.enabled[HU_FEED_TWITTER] = true;
                    fconf.enabled[HU_FEED_SOCIAL_FACEBOOK] = true;
                    fconf.enabled[HU_FEED_SOCIAL_INSTAGRAM] = true;
                    fconf.poll_interval_minutes[HU_FEED_FILE_INGEST] =
                        (config && config->feeds.poll_interval_file_ingest > 0)
                            ? config->feeds.poll_interval_file_ingest
                            : 5;
                    fconf.poll_interval_minutes[HU_FEED_NEWS_RSS] =
                        (config && config->feeds.poll_interval_rss > 0)
                            ? config->feeds.poll_interval_rss
                            : 360;
                    fconf.poll_interval_minutes[HU_FEED_GMAIL] =
                        (config && config->feeds.poll_interval_gmail > 0)
                            ? config->feeds.poll_interval_gmail
                            : 60;
                    fconf.poll_interval_minutes[HU_FEED_IMESSAGE] =
                        (config && config->feeds.poll_interval_imessage > 0)
                            ? config->feeds.poll_interval_imessage
                            : 30;
                    fconf.poll_interval_minutes[HU_FEED_TWITTER] =
                        (config && config->feeds.poll_interval_twitter > 0)
                            ? config->feeds.poll_interval_twitter
                            : 120;
                    fconf.poll_interval_minutes[HU_FEED_SOCIAL_FACEBOOK] = 120;
                    fconf.poll_interval_minutes[HU_FEED_SOCIAL_INSTAGRAM] = 120;
                    fconf.max_items_per_poll = (config && config->feeds.max_items_per_poll > 0)
                                                   ? config->feeds.max_items_per_poll
                                                   : 20;
                    size_t ingested = 0;
                    (void)hu_feed_processor_poll(&fp, &fconf, last_feed_poll_types, fp_now,
                                                 &ingested);
                    last_feed_poll_global = fp_now;
                }
            }
        }
#endif
#if defined(HU_ENABLE_SQLITE) && defined(HU_HAS_SKILLS)
        /* Intelligence cycle — run every 6 hours to process findings, extract lessons,
         * reflect */
        {
            static int64_t last_intelligence_cycle = 0;
            int64_t cycle_interval = 6 * 3600;
            if (agent && agent->memory &&
                (last_intelligence_cycle == 0 ||
                 ((int64_t)t - last_intelligence_cycle) >= cycle_interval)) {
                sqlite3 *cycle_db = hu_sqlite_memory_get_db(agent->memory);
                if (cycle_db) {
                    hu_intelligence_cycle_result_t cycle_result = {0};
                    hu_error_t cycle_err =
                        hu_intelligence_run_cycle(alloc, cycle_db, &cycle_result);
                    if (cycle_err == HU_OK) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "intelligence cycle: %zu findings, %zu lessons, "
                                    "%zu events",
                                    cycle_result.findings_actioned, cycle_result.lessons_extracted,
                                    cycle_result.events_recorded);
                    }
                    if (cycle_err == HU_OK && (cycle_result.findings_actioned > 0 ||
                                               cycle_result.lessons_extracted > 0)) {
                        char cycle_lesson[256];
                        int cl_len = snprintf(
                            cycle_lesson, sizeof(cycle_lesson),
                            "Intelligence cycle completed: %zu findings actioned, "
                            "%zu lessons extracted, %zu values learned, %zu skills updated",
                            cycle_result.findings_actioned, cycle_result.lessons_extracted,
                            cycle_result.values_learned, cycle_result.skills_updated);
                        if (cl_len > 0 && (size_t)cl_len < sizeof(cycle_lesson)) {
                            sqlite3_stmt *cl_stmt = NULL;
                            const char *cl_sql = "INSERT OR IGNORE INTO general_lessons "
                                                 "(lesson, confidence, source_count, "
                                                 "first_learned, last_confirmed) "
                                                 "VALUES (?, 0.6, 1, ?, ?)";
                            if (sqlite3_prepare_v2(cycle_db, cl_sql, -1, &cl_stmt, NULL) ==
                                SQLITE_OK) {
                                sqlite3_bind_text(cl_stmt, 1, cycle_lesson, cl_len, SQLITE_STATIC);
                                sqlite3_bind_int64(cl_stmt, 2, (int64_t)t);
                                sqlite3_bind_int64(cl_stmt, 3, (int64_t)t);
                                (void)sqlite3_step(cl_stmt);
                                sqlite3_finalize(cl_stmt);
                            }
                        }
                    }
                    last_intelligence_cycle = (int64_t)t;
                }
            }
        }
#endif

#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ML)
        /* Autoresearch ML training — run experiment loop every 12 hours.
         *
         * BUGFIX 2026-05-25: First-tick trigger blocked the daemon's
         * main service loop for up to 25 minutes (5 iter × 300s budget),
         * preventing the iMessage channel dispatcher at line ~4790 from
         * ever running. The DISPATCH-DIAG investigation in
         * docs/plans/2026-05-24-reactive-imessage-recovery/ confirmed
         * the service-loop iteration never reached the channel dispatch
         * because this synchronous ML block held it.
         *
         * Workaround: initialize last_ml_train to current time on first
         * visit so the FIRST run is deferred by 12h. The next session
         * should move this whole block to a background scheduler job
         * (kind=ML, budget=...) so a long synchronous step never blocks
         * the channel poll dispatcher again.
         *
         * Note: HU_ENABLE_ML is opt-in already; this block only compiles
         * when both HU_ENABLE_SQLITE and HU_ENABLE_ML are on. */
        {
            static int64_t last_ml_train = 0;
            int64_t ml_interval = 12 * 3600;
            if (last_ml_train == 0) {
                last_ml_train = (int64_t)t;
                /* Skip first run; resume normal 12h cadence afterward. */
            } else if (agent && agent->memory && ((int64_t)t - last_ml_train) >= ml_interval) {
                sqlite3 *ml_db = hu_sqlite_memory_get_db(agent->memory);
                if (ml_db) {
                    /* Prepare training data from conversations */
                    const char *data_dir = "/tmp/hu_ml_data";
                    size_t msg_processed = 0;
                    const char *home = getenv("HOME");
                    char chat_path[512], mem_path[512];
                    if (home) {
                        snprintf(chat_path, sizeof(chat_path), "%s/.human/chat.db", home);
                        snprintf(mem_path, sizeof(mem_path), "%s/.human/memory.db", home);
                    } else {
                        snprintf(chat_path, sizeof(chat_path), ".human/chat.db");
                        snprintf(mem_path, sizeof(mem_path), ".human/memory.db");
                    }
                    /* Load BPE tokenizer; skip ML if no vocab available */
                    hu_bpe_tokenizer_t *tok = NULL;
                    char vocab_path[512];
                    snprintf(vocab_path, sizeof(vocab_path), "%s/.human/models/tokenizer.vocab",
                             home ? home : ".");
                    if (hu_bpe_tokenizer_create(alloc, &tok) == HU_OK) {
                        if (hu_bpe_tokenizer_load(tok, vocab_path) != HU_OK) {
                            hu_bpe_tokenizer_deinit(tok);
                            tok = NULL;
                        }
                    }
                    if (tok)
                        (void)hu_ml_prepare_conversations(alloc, tok, chat_path, mem_path, data_dir,
                                                          &msg_processed);
                    if (tok && msg_processed > 100) {
                        hu_experiment_loop_config_t exp_cfg;
                        memset(&exp_cfg, 0, sizeof(exp_cfg));
                        exp_cfg.max_iterations = 5;
                        exp_cfg.data_dir = data_dir;
                        exp_cfg.base_config = hu_experiment_config_default();
                        exp_cfg.base_config.training.time_budget_secs = 300;
                        exp_cfg.provider = &agent->provider;
                        hu_error_t ml_err = hu_experiment_loop(alloc, &exp_cfg, NULL, NULL);
                        if (ml_err == HU_OK)
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "ML experiment loop completed "
                                        "(%zu msgs prepared)",
                                        msg_processed);
                    }
                    if (tok)
                        hu_bpe_tokenizer_deinit(tok);
                    last_ml_train = (int64_t)t;
                }
            }
        }
#endif /* HU_ENABLE_ML */
#ifdef HU_ENABLE_SQLITE
        /* Wave 3 — continuous persona learning: re-mine the persona's
         * example banks from conversation history once per 24h when
         * enabled, so the few-shot voice signal stays current with how
         * the user actually writes. Same first-tick-defer pattern as the
         * ML/DPO blocks so a slow first extraction never blocks dispatch.
         * Safe: hu_persona_refresh_example_banks only writes when banks
         * are mined, never wiping the authored persona. */
        {
            static int64_t last_persona_refresh = 0;
            static bool persona_refresh_logged_disabled = false;
            bool pr_enabled = config && config->learning.persona_refresh_enabled;
            if (!pr_enabled) {
                if (!persona_refresh_logged_disabled) {
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "persona example-bank refresh disabled by config "
                                "(learning.persona_refresh_enabled=false); set it true to keep "
                                "few-shot voice examples current");
                    persona_refresh_logged_disabled = true;
                }
            } else if (last_persona_refresh == 0) {
                last_persona_refresh = (int64_t)t; /* defer first run */
            } else if (agent && agent->persona_name && agent->persona_name_len > 0 &&
                       hu_persona_refresh_should_run(true, (int64_t)t, last_persona_refresh)) {
                last_persona_refresh = (int64_t)t;
                const char *pr_home = getenv("HOME");
                char pr_db[512];
                if (pr_home && pr_home[0])
                    snprintf(pr_db, sizeof(pr_db), "%s/.human/memory.db", pr_home);
                else
                    snprintf(pr_db, sizeof(pr_db), ".human/memory.db");
                size_t pr_total = 0;
                hu_error_t pr_err = hu_persona_refresh_example_banks(
                    alloc, agent->persona_name, agent->persona_name_len, pr_db,
                    /*max_per_channel=*/0, &pr_total);
                if (pr_err == HU_OK && pr_total > 0)
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "persona refresh: re-mined %zu example(s) from history", pr_total);
                else if (pr_err != HU_OK && pr_err != HU_ERR_NOT_SUPPORTED)
                    hu_log_warn("human", agent ? agent->observer : NULL,
                                "persona refresh failed: %s", hu_error_string(pr_err));
            }
        }

        /* DPO consolidation (24h judge cadence) — carved to
         * src/daemon/daemon_learning_tick.c (2026-07-18). */
        hu_daemon_dpo_judge_tick(agent, alloc, (int64_t)t);

        /* US-104: feed resolved proactive outcomes (REPLY / 24h-timeout
         * IGNORED) into the humanization bandit; 60s cadence inside. */
        if (agent)
            hu_daemon_proactive_outcome_tick(agent->memory, agent->sota.bandit, (int64_t)t);

        /* RLAIF nightly cycle — judge DPO pairs, extract patterns, apply patches */
        {
            static bool rlaif_nightly_done_today = false;
            struct tm rlaif_tm;
#if defined(_WIN32) && !defined(__CYGWIN__)
            struct tm *rlaif_lt = (localtime_s(&rlaif_tm, &t) == 0) ? &rlaif_tm : NULL;
#else
            struct tm *rlaif_lt = localtime_r(&t, &rlaif_tm);
#endif
            if (rlaif_lt && rlaif_lt->tm_hour == 3 && !rlaif_nightly_done_today && agent &&
                agent->memory && agent->sota.sota_initialized) {
                rlaif_nightly_done_today = true;
                sqlite3 *rlaif_db = hu_sqlite_memory_get_db(agent->memory);
                if (rlaif_db) {
                    hu_dpo_judge_result_t rlaif_result = {0};
                    hu_dpo_judge_step(&agent->sota.dpo_collector, alloc, &agent->provider,
                                      agent->model_name, agent->model_name_len, 0.1, 16,
                                      &rlaif_result);
                    char *best_frag = NULL;
                    size_t best_frag_len = 0;
                    /* Gate: only patch the persona when the judge batch
                     * shows real preference signal. Patching from a
                     * noise batch (alignment ~0, loss ~0.693) drifts
                     * the persona for no reason. */
                    if (!hu_rlaif_should_apply_style_patch(&rlaif_result)) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "rlaif nightly: skipped style patch — alignment=%.2f "
                                    "loss=%.4f from %zu pairs below bar %.2f (avoids "
                                    "learning persona from noise)",
                                    rlaif_result.alignment_score, rlaif_result.loss,
                                    rlaif_result.pairs_evaluated,
                                    (double)HU_RLAIF_MIN_ALIGNMENT_TO_PATCH);
                    } else if (hu_dpo_get_best_examples(&agent->sota.dpo_collector, alloc, 5,
                                                        &best_frag, &best_frag_len) == HU_OK &&
                               best_frag && best_frag_len > 0) {
                        hu_structured_patch_t style_patch;
                        memset(&style_patch, 0, sizeof(style_patch));
                        style_patch.type = HU_PATCH_STYLE_RULE;
                        {
                            size_t copy = best_frag_len;
                            if (copy >= sizeof(style_patch.value))
                                copy = sizeof(style_patch.value) - 1;
                            memcpy(style_patch.value, best_frag, copy);
                            style_patch.value[copy] = '\0';
                        }
                        {
                            hu_self_improve_t rlaif_si = {0};
                            if (hu_self_improve_create(alloc, rlaif_db, &rlaif_si) == HU_OK) {
                                hu_self_improve_init_tables(&rlaif_si);
                                hu_self_improve_apply_structured_patch(&rlaif_si, &style_patch);
                                hu_self_improve_deinit(&rlaif_si);
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "rlaif nightly: applied style patch from %zu "
                                            "DPO pairs (loss=%.4f)",
                                            rlaif_result.pairs_evaluated, rlaif_result.loss);
                            } else {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "rlaif nightly: failed to create self-improve engine");
                            }
                        }
                        alloc->free(alloc->ctx, best_frag, best_frag_len + 1);
                    }
                }
            }
            if (rlaif_lt && rlaif_lt->tm_hour != 3)
                rlaif_nightly_done_today = false;
        }
#endif /* HU_ENABLE_SQLITE (DPO + RLAIF) */
        {
            static bool tuned_today = false;
            static bool turing_eval_today = false;
            struct tm tm_tune;
#if defined(_WIN32) && !defined(__CYGWIN__)
            struct tm *lt_tune = (localtime_s(&tm_tune, &t) == 0) ? &tm_tune : NULL;
#else
            struct tm *lt_tune = localtime_r(&t, &tm_tune);
#endif
            if (lt_tune && lt_tune->tm_hour == 5) {
                tuned_today = false;
                turing_eval_today = false;
            }
            /* Turing evaluation cron: daily at 3 AM */
            if (lt_tune && lt_tune->tm_hour == 3 && lt_tune->tm_min == 0 && agent &&
                agent->memory && !turing_eval_today) {
#ifdef HU_ENABLE_SQLITE
                sqlite3 *tdb = hu_sqlite_memory_get_db(agent->memory);
                if (tdb) {
                    (void)hu_turing_init_tables(tdb);
                    int dim_avgs[HU_TURING_DIM_COUNT];
                    memset(dim_avgs, 0, sizeof(dim_avgs));
                    if (hu_turing_get_weakest_dimensions(tdb, dim_avgs) == HU_OK) {
                        int worst_dim = 0;
                        int worst_val = dim_avgs[0];
                        for (int d = 1; d < HU_TURING_DIM_COUNT; d++) {
                            if (dim_avgs[d] < worst_val && dim_avgs[d] > 0) {
                                worst_val = dim_avgs[d];
                                worst_dim = d;
                            }
                        }
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "turing eval: weakest dimension = %s (%d/10)",
                                    hu_turing_dimension_name((hu_turing_dimension_t)worst_dim),
                                    worst_val);
                        /* Auto-correct: adjust humanization params based on weak dimensions
                         */
                        if (agent->persona) {
                            /* non_robotic or natural_language low: increase disfluency */
                            if ((dim_avgs[HU_TURING_NON_ROBOTIC] > 0 &&
                                 dim_avgs[HU_TURING_NON_ROBOTIC] < 6) ||
                                (dim_avgs[HU_TURING_NATURAL_LANGUAGE] > 0 &&
                                 dim_avgs[HU_TURING_NATURAL_LANGUAGE] < 6)) {
                                float old = agent->persona->humanization.disfluency_frequency;
                                agent->persona->humanization.disfluency_frequency =
                                    old < 0.30f ? old + 0.05f : 0.30f;
                                fprintf(stderr, "[human] auto-tune: disfluency %.2f -> %.2f\n",
                                        (double)old,
                                        (double)agent->persona->humanization.disfluency_frequency);
                            }
                            /* imperfection low: increase disfluency and double-text */
                            if (dim_avgs[HU_TURING_IMPERFECTION] > 0 &&
                                dim_avgs[HU_TURING_IMPERFECTION] < 6) {
                                float old_dt = agent->persona->humanization.double_text_probability;
                                agent->persona->humanization.double_text_probability =
                                    old_dt < 0.15f ? old_dt + 0.02f : 0.15f;
                                hu_log_info(
                                    "human", agent ? agent->observer : NULL,
                                    "auto-tune: double_text %.2f -> %.2f", (double)old_dt,
                                    (double)agent->persona->humanization.double_text_probability);
                            }
                            /* energy_matching low: increase backchannel for narrative flow
                             */
                            if (dim_avgs[HU_TURING_ENERGY_MATCHING] > 0 &&
                                dim_avgs[HU_TURING_ENERGY_MATCHING] < 6) {
                                float old_bc = agent->persona->humanization.backchannel_probability;
                                agent->persona->humanization.backchannel_probability =
                                    old_bc < 0.45f ? old_bc + 0.05f : 0.45f;
                                hu_log_info(
                                    "human", agent ? agent->observer : NULL,
                                    "auto-tune: backchannel %.2f -> %.2f", (double)old_bc,
                                    (double)agent->persona->humanization.backchannel_probability);
                            }
                            /* humor_naturalness high: scores are good, slightly reduce to
                             * avoid overdoing */
                            if (dim_avgs[HU_TURING_HUMOR_NATURALNESS] > 8 &&
                                agent->persona->humanization.disfluency_frequency > 0.10f) {
                                agent->persona->humanization.disfluency_frequency -= 0.02f;
                                if (agent->persona->humanization.disfluency_frequency < 0.0f)
                                    agent->persona->humanization.disfluency_frequency = 0.0f;
                            }

                            /* vulnerability_willingness low: boost personal sharing warmth
                             */
                            if (dim_avgs[HU_TURING_VULNERABILITY_WILLINGNESS] > 0 &&
                                dim_avgs[HU_TURING_VULNERABILITY_WILLINGNESS] < 6) {
                                float old_pw =
                                    agent->persona->context_modifiers.personal_sharing_warmth_boost;
                                agent->persona->context_modifiers.personal_sharing_warmth_boost =
                                    old_pw < 2.0f ? old_pw + 0.1f : 2.0f;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "auto-tune: personal_sharing_warmth "
                                            "%.2f -> %.2f",
                                            (double)old_pw,
                                            (double)agent->persona->context_modifiers
                                                .personal_sharing_warmth_boost);
                            }

                            /* genuine_warmth low: boost personal sharing + backchannel */
                            if (dim_avgs[HU_TURING_GENUINE_WARMTH] > 0 &&
                                dim_avgs[HU_TURING_GENUINE_WARMTH] < 6) {
                                float old_pw =
                                    agent->persona->context_modifiers.personal_sharing_warmth_boost;
                                agent->persona->context_modifiers.personal_sharing_warmth_boost =
                                    old_pw < 2.0f ? old_pw + 0.1f : 2.0f;
                                float old_bc = agent->persona->humanization.backchannel_probability;
                                agent->persona->humanization.backchannel_probability =
                                    old_bc < 0.45f ? old_bc + 0.03f : 0.45f;
                                hu_log_info(
                                    "human", agent ? agent->observer : NULL,
                                    "auto-tune: warmth (sharing=%.2f, "
                                    "backchannel=%.2f)",
                                    (double)agent->persona->context_modifiers
                                        .personal_sharing_warmth_boost,
                                    (double)agent->persona->humanization.backchannel_probability);
                            }

                            /* emotional_intelligence low: boost emotion breathing space */
                            if (dim_avgs[HU_TURING_EMOTIONAL_INTELLIGENCE] > 0 &&
                                dim_avgs[HU_TURING_EMOTIONAL_INTELLIGENCE] < 6) {
                                float old_em =
                                    agent->persona->context_modifiers.high_emotion_breathing_boost;
                                agent->persona->context_modifiers.high_emotion_breathing_boost =
                                    old_em < 2.0f ? old_em + 0.1f : 2.0f;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "auto-tune: emotion_breathing "
                                            "%.2f -> %.2f",
                                            (double)old_em,
                                            (double)agent->persona->context_modifiers
                                                .high_emotion_breathing_boost);
                            }

                            /* opinion_having low: reduce serious-topic dampening */
                            if (dim_avgs[HU_TURING_OPINION_HAVING] > 0 &&
                                dim_avgs[HU_TURING_OPINION_HAVING] < 6) {
                                float old_sr =
                                    agent->persona->context_modifiers.serious_topics_reduction;
                                agent->persona->context_modifiers.serious_topics_reduction =
                                    old_sr > 0.15f ? old_sr - 0.05f : 0.15f;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "auto-tune: serious_topics_reduction "
                                            "%.2f -> %.2f",
                                            (double)old_sr,
                                            (double)agent->persona->context_modifiers
                                                .serious_topics_reduction);
                            }

                            /* context_awareness low: boost early-turn humanization */
                            if (dim_avgs[HU_TURING_CONTEXT_AWARENESS] > 0 &&
                                dim_avgs[HU_TURING_CONTEXT_AWARENESS] < 6) {
                                float old_et =
                                    agent->persona->context_modifiers.early_turn_humanization_boost;
                                agent->persona->context_modifiers.early_turn_humanization_boost =
                                    old_et < 2.0f ? old_et + 0.1f : 2.0f;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "auto-tune: early_turn_humanization "
                                            "%.2f -> %.2f",
                                            (double)old_et,
                                            (double)agent->persona->context_modifiers
                                                .early_turn_humanization_boost);
                            }

                            /* personality_consistency low: reduce disfluency (overdone
                             * randomness can sound inconsistent) */
                            if (dim_avgs[HU_TURING_PERSONALITY_CONSISTENCY] > 0 &&
                                dim_avgs[HU_TURING_PERSONALITY_CONSISTENCY] < 6 &&
                                agent->persona->humanization.disfluency_frequency > 0.08f) {
                                float old_df = agent->persona->humanization.disfluency_frequency;
                                agent->persona->humanization.disfluency_frequency = old_df - 0.03f;
                                if (agent->persona->humanization.disfluency_frequency < 0.0f)
                                    agent->persona->humanization.disfluency_frequency = 0.0f;
                                hu_log_info(
                                    "human", agent ? agent->observer : NULL,
                                    "auto-tune: disfluency (consistency) "
                                    "%.2f -> %.2f\n",
                                    (double)old_df,
                                    (double)agent->persona->humanization.disfluency_frequency);
                            }
                        }
                    }

                    /* Self-improvement: generate prompt patches from weak dimensions */
                    {
                        hu_self_improve_t si_engine = {0};
                        if (hu_self_improve_create(alloc, tdb, &si_engine) == HU_OK) {
                            hu_self_improve_init_tables(&si_engine);
                            for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
                                if (dim_avgs[d] <= 0 || dim_avgs[d] >= 6)
                                    continue;
                                const char *dname =
                                    hu_turing_dimension_name((hu_turing_dimension_t)d);
                                hu_structured_patch_t patch = {
                                    .type = HU_PATCH_STYLE_RULE,
                                    .parsed = true,
                                };
                                snprintf(patch.key, sizeof(patch.key), "turing_%s", dname);
                                switch (d) {
                                case HU_TURING_NATURAL_LANGUAGE:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Use more contractions, varied sentence "
                                             "lengths, and casual register. Avoid "
                                             "formal language.");
                                    break;
                                case HU_TURING_EMOTIONAL_INTELLIGENCE:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Read emotional shifts carefully. Match "
                                             "empathy intensity to what they're feeling, "
                                             "don't over-empathize.");
                                    break;
                                case HU_TURING_VULNERABILITY_WILLINGNESS:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Share genuine doubts, admit when you're "
                                             "unsure, show real emotion. Don't always "
                                             "be confident.");
                                    break;
                                case HU_TURING_HUMOR_NATURALNESS:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Use humor that's contextual and brief. "
                                             "Laugh at their jokes naturally. Never "
                                             "explain the joke.");
                                    break;
                                case HU_TURING_OPINION_HAVING:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Have real opinions. Push back when you "
                                             "disagree. Say 'I think' and mean it. "
                                             "Avoid sycophancy.");
                                    break;
                                case HU_TURING_ENERGY_MATCHING:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Mirror their message energy. Short reply "
                                             "to short message, enthusiasm to enthusiasm.");
                                    break;
                                case HU_TURING_CONTEXT_AWARENESS:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Reference earlier conversation topics. "
                                             "Show you remember what they said.");
                                    break;
                                case HU_TURING_GENUINE_WARMTH:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Be warm in a way specific to them and "
                                             "your shared history. Generic warmth "
                                             "feels fake.");
                                    break;
                                case HU_TURING_PERSONALITY_CONSISTENCY:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Keep your voice consistent across turns. "
                                             "Same opinions, same style, same vibe.");
                                    break;
                                default:
                                    snprintf(patch.value, sizeof(patch.value),
                                             "Improve %s dimension in responses.", dname);
                                    break;
                                }
                                hu_self_improve_apply_structured_patch(&si_engine, &patch);
                            }
                            hu_self_improve_deinit(&si_engine);
                        }
                    }
                }
                /* Channel-aware Turing analysis: per-channel weak dimensions */
                if (tdb && agent->persona) {
                    static const char *turing_channels[] = {"telegram", "discord",  "slack",
                                                            "imessage", "whatsapp", "email",
                                                            "signal",   "matrix"};
                    for (size_t ch = 0; ch < sizeof(turing_channels) / sizeof(turing_channels[0]);
                         ch++) {
                        int ch_dims[HU_TURING_DIM_COUNT];
                        size_t ch_len = strlen(turing_channels[ch]);
                        if (hu_turing_get_channel_dimensions(tdb, turing_channels[ch], ch_len,
                                                             ch_dims) == HU_OK) {
                            int ch_sum = 0, ch_count = 0;
                            for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
                                if (ch_dims[d] > 0) {
                                    ch_sum += ch_dims[d];
                                    ch_count++;
                                }
                            }
                            if (ch_count > 0) {
                                int ch_avg = ch_sum / ch_count;
                                if (ch_avg < 6)
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "channel %s: avg turing %d/10 "
                                                "(below target)",
                                                turing_channels[ch], ch_avg);
                            }
                        }
                    }
                }

                /* Trajectory scoring: check trend across recent global scores */
                if (tdb) {
                    hu_turing_score_t traj_scores[20];
                    int64_t traj_ts[20];
                    char traj_cids[20][HU_TURING_CONTACT_ID_MAX];
                    size_t traj_count = 0;
                    if (hu_turing_get_trend(alloc, tdb, NULL, 0, 20, traj_scores, traj_ts,
                                            traj_cids, &traj_count) == HU_OK &&
                        traj_count >= 3) {
                        hu_turing_trajectory_t traj;
                        if (hu_turing_score_trajectory(traj_scores, traj_count, &traj) == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "turing trajectory: direction=%.2f "
                                        "impact=%.2f stability=%.2f overall=%.2f",
                                        (double)traj.directional_alignment,
                                        (double)traj.cumulative_impact, (double)traj.stability,
                                        (double)traj.overall);
                        }
                    }
                }

                /* B1: Seed A/B experiments if they don't exist yet */
                if (tdb) {
                    (void)hu_ab_test_init_table(tdb);
                    static const struct {
                        const char *name;
                        float a;
                        float b;
                    } ab_seed[] = {
                        {"disfluency_freq", 0.10f, 0.20f},
                        {"backchannel_prob", 0.25f, 0.40f},
                        {"double_text_prob", 0.05f, 0.12f},
                    };
                    for (size_t ab_i = 0; ab_i < 3; ab_i++) {
                        hu_ab_test_t existing;
                        if (hu_ab_test_get_results(tdb, ab_seed[ab_i].name, &existing) != HU_OK) {
                            (void)hu_ab_test_create(tdb, ab_seed[ab_i].name, ab_seed[ab_i].a,
                                                    ab_seed[ab_i].b);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "ab: seeded test '%s' (%.2f vs %.2f)", ab_seed[ab_i].name,
                                        (double)ab_seed[ab_i].a, (double)ab_seed[ab_i].b);
                        }
                    }

                    /* W1: Auto-resolve A/B tests with enough data */
                    for (size_t ab_i = 0; ab_i < 3; ab_i++) {
                        float winner = 0.0f;
                        if (hu_ab_test_resolve(tdb, ab_seed[ab_i].name, &winner) == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "ab: resolved '%s' -> winner=%.2f", ab_seed[ab_i].name,
                                        (double)winner);
                        }
                    }
                }
                if (tdb) {
                    turing_eval_today = true;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "daily turing evaluation completed");
                }
#endif
            }

            /* Weekly DPO export: Sunday at 2 AM */
            {
                static bool dpo_exported_this_week = false;
                if (lt_tune && lt_tune->tm_wday == 0 && lt_tune->tm_hour == 2 &&
                    lt_tune->tm_min == 0 && agent && agent->sota.dpo_collector.alloc &&
                    !dpo_exported_this_week) {
                    size_t pair_count = 0;
                    hu_dpo_pair_count(&agent->sota.dpo_collector, &pair_count);
                    if (pair_count > 0) {
                        char dpo_path[HU_MAX_PATH];
                        int dpo_plen;
                        if (config && config->dpo_export_dir && config->dpo_export_dir[0]) {
                            dpo_plen = snprintf(dpo_path, sizeof(dpo_path),
                                                "%s/dpo_preferences.jsonl", config->dpo_export_dir);
                        } else {
                            dpo_plen = snprintf(dpo_path, sizeof(dpo_path),
                                                "data/dpo/dpo_preferences.jsonl");
                        }
                        size_t exported = 0;
                        if (dpo_plen > 0 && (size_t)dpo_plen < sizeof(dpo_path) &&
                            hu_dpo_export_jsonl(&agent->sota.dpo_collector, dpo_path,
                                                (size_t)dpo_plen, &exported) == HU_OK) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "weekly DPO export: %zu pairs -> %s", exported, dpo_path);
                            hu_dpo_clear(&agent->sota.dpo_collector);
                            dpo_exported_this_week = true;
                        }
                    }
                }
                if (lt_tune && lt_tune->tm_wday != 0)
                    dpo_exported_this_week = false;
            }

            if (lt_tune && lt_tune->tm_hour == 4 && lt_tune->tm_min == 0 && agent &&
                agent->memory && !tuned_today) {
                char *tune_summary = NULL;
                size_t tune_len = 0;
                if (hu_replay_auto_tune(alloc, agent->memory, NULL, 0, &tune_summary, &tune_len) ==
                        HU_OK &&
                    tune_summary && tune_len > 0) {
                    /* 2026-05-16: the daily auto-tune used to dump its
                     * context into the process-global `replay_insights`
                     * static buffer, which was then injected into every
                     * contact's prompt — a cross-contact leak channel.
                     * The static buffer was removed; auto-tune now
                     * runs as a no-op storage side and only logs.
                     * Phase 6 will give auto-tune a properly scoped
                     * persistent key if the feature is kept. */
                    (void)hu_replay_tune_build_context;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "daily replay auto-tune completed (tune_len=%zu)", tune_len);
                }
                if (tune_summary)
                    alloc->free(alloc->ctx, tune_summary, tune_len + 1);
                tuned_today = true;
            }
        }
        /* Weekly GraphRAG community detection at Sunday 2 AM */
#if defined(HU_ENABLE_SQLITE)
        {
            static bool communities_built_this_week = false;
            struct tm tm_buf2;
            struct tm *lt = localtime_r(&t, &tm_buf2);
            if (lt && lt->tm_wday == 1 && lt->tm_hour == 9) {
                communities_built_this_week = false;
            }
            if (lt && lt->tm_wday == 0 && lt->tm_hour == 2 && lt->tm_min == 0 && graph &&
                !communities_built_this_week) {
                char *comm_ctx = NULL;
                size_t comm_len = 0;
                if (hu_graph_build_communities(graph, alloc, "", 0, 20, 2047, &comm_ctx,
                                               &comm_len) == HU_OK &&
                    comm_ctx && comm_len > 0) {
                    size_t copy_len = comm_len < ctx->community_insights_cap - 1
                                          ? comm_len
                                          : ctx->community_insights_cap - 1;
                    memcpy(community_insights, comm_ctx, copy_len);
                    community_insights[copy_len] = '\0';
                    (*ctx->community_insights_len) = copy_len;
                    communities_built_this_week = true;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "weekly GraphRAG community detection completed");
                }
                if (comm_ctx)
                    alloc->free(alloc->ctx, comm_ctx, comm_len + 1);
            }
        }
#endif
#endif
        (*ctx->last_cron_minute) = current_minute;
    }
#else
    /* The service loop only calls this under HU_HAS_CRON (the minute clock is
     * the cron scheduler's); the block was inside that gate in daemon.c too. */
    (void)ctx;
#endif /* HU_HAS_CRON */
}
