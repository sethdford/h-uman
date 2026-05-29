/*
 * intrinsic_drive.c — bounded intrinsic motivation (A3). Pure drive dynamics +
 * the safety-bearing start predicate + self-originated goal. See
 * include/human/agent/intrinsic_drive.h and
 * docs/plans/2026-05-29-intrinsic-motivation/.
 *
 * SAFETY: internal + propose-only, no action surface. The start predicate is
 * the load-bearing guarantee (preemption / budget / rate) and is pure.
 */

#include "human/agent/intrinsic_drive.h"

#include <stdio.h>

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

void hu_intrinsic_drive_tick(hu_intrinsic_drive_t *d, bool had_user_activity, int64_t now) {
    if (!d)
        return;
    if (had_user_activity) {
        d->boredom = clamp01(d->boredom - 0.3);
        d->curiosity = clamp01(d->curiosity - 0.1);
        d->last_user_ts = now;
    } else {
        d->boredom = clamp01(d->boredom + 0.1);
        d->curiosity = clamp01(d->curiosity + 0.05);
    }
}

double hu_intrinsic_drive_level(const hu_intrinsic_drive_t *d) {
    if (!d)
        return 0.0;
    return d->curiosity > d->boredom ? d->curiosity : d->boredom;
}

bool hu_intrinsic_should_start(const hu_intrinsic_start_facts_t *f) {
    if (!f)
        return false;
    /* Hard preemption: a user turn in flight always wins. */
    if (f->user_active)
        return false;
    /* Hard budget. */
    if (f->budget_tokens_remaining < HU_INTRINSIC_MIN_BUDGET_TOKENS)
        return false;
    /* Be quiet for a while before doing anything unprompted. */
    if (f->secs_since_user < HU_INTRINSIC_MIN_QUIET_SECS)
        return false;
    /* Rate limit: don't pile intrinsic actions back to back. */
    if (f->secs_since_intrinsic < HU_INTRINSIC_MIN_INTERVAL_SECS)
        return false;
    /* Only when the drive is genuinely high. */
    if (f->drive_level < HU_INTRINSIC_DRIVE_THRESHOLD)
        return false;
    return true;
}

void hu_intrinsic_make_goal(const hu_intrinsic_drive_t *d, hu_intrinsic_goal_t *out) {
    if (!out)
        return;
    out->origin = "intrinsic_curiosity";
    const char *driver = (d && d->curiosity >= d->boredom) ? "curiosity" : "restlessness";
    /* Generic, internal, propose-only framing — NOT a user task. */
    snprintf(out->description, sizeof(out->description),
             "Follow my own %s: explore something I find interesting, "
             "then decide later whether it is worth sharing.",
             driver);
}

bool hu_intrinsic_may_share(double proposer_confidence) {
    return proposer_confidence >= HU_INTRINSIC_SHARE_MIN_CONFIDENCE;
}

#include "human/core/log.h"
#include <stdatomic.h>

void hu_intrinsic_run_tick(hu_intrinsic_drive_t *drive, const hu_intrinsic_runtime_cfg_t *cfg,
                           const hu_intrinsic_start_facts_t *facts, hu_observer_t *obs, int64_t now,
                           hu_intrinsic_tick_result_t *out) {
    if (out) {
        out->outcome = HU_INTRINSIC_TICK_DISABLED;
        out->goal.description[0] = '\0';
        out->goal.origin = NULL;
        out->audit[0] = '\0';
    }

    /* AC-8: config gate, default-off, with a one-shot disabled log that names
     * the config key (silent-config-gated-subsystems rule). */
    if (!cfg || !cfg->enabled) {
        static atomic_bool warned_intrinsic_off = false;
        hu_log_info_once(&warned_intrinsic_off, "human", obs,
                         "intrinsic motivation disabled (cfg.intrinsic.enabled=false) — set "
                         "intrinsic.enabled=true in config.json to activate the curiosity loop");
        return;
    }
    if (!out)
        return;
    out->outcome = HU_INTRINSIC_TICK_SKIPPED;
    if (!facts)
        return;

    /* AC-3: hard per-tick budget cap (operator-tunable). */
    uint32_t tick_budget =
        cfg->per_tick_token_budget ? cfg->per_tick_token_budget : HU_INTRINSIC_DEFAULT_TICK_BUDGET;
    if (facts->budget_tokens_remaining < tick_budget)
        return;

    /* AC-4/AC-6: the pure start predicate (preemption / quiet / rate / drive). */
    if (!hu_intrinsic_should_start(facts))
        return;

    hu_intrinsic_make_goal(drive, &out->goal);
    if (drive)
        drive->last_intrinsic_ts = now;

    /* AC-7: auditable — record origin/trigger/outcome in the result (testable)
     * AND the operator log. */
    snprintf(out->audit, sizeof(out->audit),
             "origin=%s trigger=drive=%.2f,quiet=%llds outcome=started",
             out->goal.origin ? out->goal.origin : "intrinsic_curiosity", facts->drive_level,
             (long long)facts->secs_since_user);
    hu_log_info("human", obs, "intrinsic action — %s", out->audit);
    out->outcome = HU_INTRINSIC_TICK_STARTED;
}
