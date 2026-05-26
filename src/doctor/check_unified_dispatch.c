/* src/doctor/check_unified_dispatch.c
 *
 * M3 Dispatch — doctor check for unified-dispatch health.
 *
 * Reads the Sprint 41 follow-up #3 retry-outcome telemetry from
 * hu_guard_reject_stats_snapshot() and reports operator-readable
 * verdict + raw counters + rescue-rate math. See header for verdict
 * semantics and the rationale for each threshold.
 *
 * Companion to scripts/dpo-rejections-summary.py: the script reads
 * the on-disk DPO log; this check reads the in-process atomics. Both
 * answer "is the unified dispatch path healthy?" from different
 * sources, and together they triangulate. */

#include "human/doctor/check_unified_dispatch.h"

#include "human/agent/response_guard.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <string.h>

/* Static buffers per the check.h contract (the caller's result
 * borrows our string pointers; they must stay live across the
 * registry_run_all loop). */
static char s_reason_buf[512];
static char s_detail_json_buf[1024];

/* Thresholds chosen for operator-readable interpretation. Tunable
 * later from production data without touching the verdict shape. */
#define HU_UD_MIN_SAMPLES_FOR_RATE 50
#define HU_UD_HEALTHY_RESCUE_RATE  0.80
#define HU_UD_FAIL_RESCUE_RATE     0.50

static hu_doctor_check_result_t check_unified_dispatch_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx; /* this check has no ctx — counters are process-wide */

    hu_guard_reject_stats_t s;
    memset(&s, 0, sizeof(s));
    hu_guard_reject_stats_snapshot(&s);

    /* Total retry outcomes for G9 only — distinguishes from the
     * other detectors which have their own counters (semantic_leak,
     * length_anomaly, etc.). The retry-outcome telemetry is G9-
     * specific because the retry path is wired in agent_turn.c
     * only when G9 was the original rejection. */
    uint64_t total = s.g9_retry_rescued + s.g9_retry_thrashed + s.g9_retry_starved;

    /* Detail JSON has the raw counts AND the derived rate so
     * downstream parsers (e.g. operator dashboards) don't have to
     * recompute. Total of all detector rejections is also included
     * for cross-reference with the DPO log file size. */
    snprintf(s_detail_json_buf, sizeof(s_detail_json_buf),
             "{\"g9_retry_rescued\":%llu,"
             "\"g9_retry_thrashed\":%llu,"
             "\"g9_retry_starved\":%llu,"
             "\"g9_retry_total\":%llu,"
             "\"rescue_rate\":%.3f,"
             "\"all_detector_rejections\":{"
             "\"semantic_leak\":%llu,"
             "\"length_anomaly\":%llu,"
             "\"director_echo\":%llu,"
             "\"persona_pii_echo\":%llu,"
             "\"persona_identity_echo\":%llu,"
             "\"naked_discourse_opener\":%llu"
             "}}",
             (unsigned long long)s.g9_retry_rescued, (unsigned long long)s.g9_retry_thrashed,
             (unsigned long long)s.g9_retry_starved, (unsigned long long)total,
             total > 0 ? ((double)s.g9_retry_rescued / (double)total) : 0.0,
             (unsigned long long)s.semantic_leak, (unsigned long long)s.length_anomaly,
             (unsigned long long)s.director_echo, (unsigned long long)s.persona_pii_echo,
             (unsigned long long)s.persona_identity_echo,
             (unsigned long long)s.naked_discourse_opener);

    /* NA — no data. Most common case at daemon startup or low-traffic
     * deployments. Don't WARN on this; it's informational. */
    if (total == 0) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "no G9 retry-outcome data yet (rescued=0, thrashed=0, starved=0). "
                 "Daemon may not have run long enough, or no Jordan-class drafts "
                 "have triggered G9. Check %llu total all-detector rejections.",
                 (unsigned long long)(s.semantic_leak + s.length_anomaly + s.director_echo +
                                      s.persona_pii_echo + s.persona_identity_echo +
                                      s.naked_discourse_opener));
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }

    double rate = (double)s.g9_retry_rescued / (double)total;

    /* WARN — low sample size. Rate is meaningless on small N; the
     * detail JSON carries the raw counts so operators can decide
     * whether to wait. */
    if (total < HU_UD_MIN_SAMPLES_FOR_RATE) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "low signal: only %llu G9 retry outcomes (rate=%.1f%% but "
                 "n<%d so the rate is noisy). Watch for more data.",
                 (unsigned long long)total, rate * 100.0, HU_UD_MIN_SAMPLES_FOR_RATE);
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }

    /* FAIL — rescue rate too low. Either LoRA is severely stuck OR
     * contacts are being silently starved. Either way: act. */
    if (rate < HU_UD_FAIL_RESCUE_RATE) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "rescue rate %.1f%% < %.0f%% threshold over %llu outcomes "
                 "(rescued=%llu, thrashed=%llu, starved=%llu). LoRA likely stuck — "
                 "retrain on DPO negatives, or use per-channel G9 disable to mitigate.",
                 rate * 100.0, HU_UD_FAIL_RESCUE_RATE * 100.0, (unsigned long long)total,
                 (unsigned long long)s.g9_retry_rescued, (unsigned long long)s.g9_retry_thrashed,
                 (unsigned long long)s.g9_retry_starved);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_buf, s_detail_json_buf};
    }

    /* WARN — middling rate. G9 is helping but ~1/4 to 1/2 of retries
     * produce more bad text. Adapter is partially stuck. */
    if (rate < HU_UD_HEALTHY_RESCUE_RATE) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "rescue rate %.1f%% over %llu outcomes — between %.0f%% and %.0f%%; "
                 "G9 is helping but the adapter is partially stuck "
                 "(thrashed=%llu of %llu = %.1f%% retries also bad). "
                 "Consider retraining if this persists.",
                 rate * 100.0, (unsigned long long)total, HU_UD_FAIL_RESCUE_RATE * 100.0,
                 HU_UD_HEALTHY_RESCUE_RATE * 100.0, (unsigned long long)s.g9_retry_thrashed,
                 (unsigned long long)total, 100.0 * (double)s.g9_retry_thrashed / (double)total);
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }

    /* PASS — healthy steady state. G9 is catching real bad drafts
     * and the retry is producing clean text most of the time. */
    snprintf(s_reason_buf, sizeof(s_reason_buf),
             "unified dispatch healthy: rescue rate %.1f%% over %llu G9 outcomes "
             "(rescued=%llu, thrashed=%llu, starved=%llu).",
             rate * 100.0, (unsigned long long)total, (unsigned long long)s.g9_retry_rescued,
             (unsigned long long)s.g9_retry_thrashed, (unsigned long long)s.g9_retry_starved);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason_buf, s_detail_json_buf};
}

hu_doctor_check_t hu_doctor_check_unified_dispatch = {
    .name = "unified_dispatch",
    .description = "Reports M3 unified-dispatch G9 retry-outcome health (rescued/thrashed/starved)",
    .run = check_unified_dispatch_run,
    .fix = NULL, /* No autofix — operator action depends on the specific verdict */
    .user_data = NULL,
};
