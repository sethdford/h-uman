/* src/doctor/check_outbound_stats.c
 *
 * Sprint 60 — doctor check that exposes outbound pipeline stats.
 * See header for contract.
 *
 * The check is informational — always PASS. The interesting payload
 * is in detail_json. Doctor's --json flag surfaces it for operators
 * + dashboards.
 */

#include "human/doctor/check_outbound_stats.h"
#include "human/agent/outbound_stats.h"
#include "human/doctor/check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Reasonable upper bound for the detail JSON:
 *   7 stages * ~120 bytes per stage entry = ~840
 *   + overall wrapper + totals = ~200
 *   + Sprint 60 follow-up health fields (reject_rate, healthy,
 *     warnings) = ~150
 *   = ~1200 bytes total. Round to 2KB to leave headroom for future
 *     fields without re-tuning. */
#define HU_DOCTOR_OUTBOUND_STATS_JSON_BUF 2048

/* Health-metric thresholds — Sprint 60 follow-up. These choices are
 * documented in tests/test_doctor_outbound_stats.c via the cases that
 * pin them; operators can tune by editing here + rerunning the suite.
 *
 *   REJECT_RATE_THRESHOLD — fraction of (reject / total). 0.25 = 25%.
 *     Above this AND past the sample minimum, the check emits a
 *     "reject_rate_high" warning so dashboards can flag.
 *
 *   MIN_SAMPLE — minimum total records before reject_rate is
 *     considered meaningful. Without this floor, a fresh deploy with
 *     one adversarial test send would trip the warning permanently
 *     at 100% reject rate. */
#define HU_DOCTOR_OUTBOUND_STATS_REJECT_RATE_THRESHOLD 0.25
#define HU_DOCTOR_OUTBOUND_STATS_MIN_SAMPLE            100u

/* Per-call static buffer. The doctor result.detail_json is a
 * borrowed pointer; the check vtable owns the buffer. Single-threaded
 * use is fine because doctor runs checks sequentially. */
static char s_detail_buf[HU_DOCTOR_OUTBOUND_STATS_JSON_BUF];

size_t hu_doctor_check_outbound_stats_render_json(const struct hu_outbound_stats_snapshot *snap,
                                                  char *buf, size_t cap) {
    if (!snap || !buf || cap == 0)
        return 0;
    /* Per-verdict totals across all stages, computed inline. */
    uint64_t totals[HU_OUTBOUND_STATS_VERDICT_COUNT] = {0};
    for (size_t s = 0; s < HU_OUTBOUND_STATS_STAGE_COUNT; s++) {
        for (size_t v = 0; v < HU_OUTBOUND_STATS_VERDICT_COUNT; v++) {
            totals[v] += snap->counts[s][v];
        }
    }

    size_t off = 0;
    int n = snprintf(buf + off, cap - off, "{\"stages\":[");
    if (n < 0 || (size_t)n >= cap - off)
        return 0;
    off += (size_t)n;

    for (size_t s = 0; s < HU_OUTBOUND_STATS_STAGE_COUNT; s++) {
        const char *stage_name = hu_outbound_stats_stage_name((hu_outbound_stats_stage_t)s);
        n = snprintf(buf + off, cap - off,
                     "%s{\"name\":\"%s\",\"send\":%llu,\"rewrite\":%llu,"
                     "\"regenerate\":%llu,\"reject\":%llu}",
                     s == 0 ? "" : ",", stage_name, (unsigned long long)snap->counts[s][0],
                     (unsigned long long)snap->counts[s][1], (unsigned long long)snap->counts[s][2],
                     (unsigned long long)snap->counts[s][3]);
        if (n < 0 || (size_t)n >= cap - off)
            return 0;
        off += (size_t)n;
    }

    n = snprintf(buf + off, cap - off,
                 "],\"total_send\":%llu,\"total_rewrite\":%llu,"
                 "\"total_regenerate\":%llu,\"total_reject\":%llu",
                 (unsigned long long)totals[0], (unsigned long long)totals[1],
                 (unsigned long long)totals[2], (unsigned long long)totals[3]);
    if (n < 0 || (size_t)n >= cap - off)
        return 0;
    off += (size_t)n;

    /* Sprint 60 follow-up — derived health metrics for dashboards.
     *
     *   reject_rate   = total_reject / (sum of all verdicts).
     *                   0.00 when there's been no traffic yet.
     *   healthy       = false ONLY when one of the warnings fires.
     *   warnings      = array of stable identifier strings; operators
     *                   alert on entries, dashboards chart trend.
     *
     * The OTHER bucket's nonzero count means the pipeline emitted a
     * stage name the stats subsystem doesn't recognize — likely a new
     * stage was added without updating the name-table in stats.c. We
     * surface this as "unknown_stage_counts" so the typo is visible. */
    uint64_t total_records = totals[0] + totals[1] + totals[2] + totals[3];
    double reject_rate = (total_records == 0) ? 0.0 : (double)totals[3] / (double)total_records;
    uint64_t other_bucket_sum = 0;
    for (size_t v = 0; v < HU_OUTBOUND_STATS_VERDICT_COUNT; v++)
        other_bucket_sum += snap->counts[HU_OUTBOUND_STATS_STAGE_OTHER][v];
    bool warn_rate = (total_records >= HU_DOCTOR_OUTBOUND_STATS_MIN_SAMPLE) &&
                     (reject_rate > HU_DOCTOR_OUTBOUND_STATS_REJECT_RATE_THRESHOLD);
    bool warn_other = (other_bucket_sum > 0);
    bool healthy = !warn_rate && !warn_other;

    /* Build the warnings array — comma-separated, only present
     * conditions get listed. */
    char warnings_buf[128];
    size_t wpos = 0;
    warnings_buf[0] = '\0';
    if (warn_rate) {
        int wn = snprintf(warnings_buf + wpos, sizeof(warnings_buf) - wpos, "\"reject_rate_high\"");
        if (wn > 0 && (size_t)wn < sizeof(warnings_buf) - wpos)
            wpos += (size_t)wn;
    }
    if (warn_other) {
        int wn = snprintf(warnings_buf + wpos, sizeof(warnings_buf) - wpos,
                          "%s\"unknown_stage_counts\"", wpos > 0 ? "," : "");
        if (wn > 0 && (size_t)wn < sizeof(warnings_buf) - wpos)
            wpos += (size_t)wn;
    }

    n = snprintf(buf + off, cap - off, ",\"reject_rate\":%.2f,\"healthy\":%s,\"warnings\":[%s]}",
                 reject_rate, healthy ? "true" : "false", warnings_buf);
    if (n < 0 || (size_t)n >= cap - off)
        return 0;
    off += (size_t)n;
    return off;
}

static hu_doctor_check_result_t outbound_stats_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx;
    hu_outbound_stats_snapshot_t snap = {0};
    hu_doctor_check_result_t result = {.verdict = HU_DOCTOR_PASS,
                                       .reason = "outbound pipeline stats snapshot",
                                       .detail_json = NULL};
    if (hu_outbound_stats_snapshot(&snap) != HU_OK) {
        /* Should never happen — snapshot only fails on NULL out, which
         * we don't pass. Defensive: report NA so the check doesn't
         * silently become FAIL on some future API breakage. */
        result.verdict = HU_DOCTOR_NA;
        result.reason = "outbound stats snapshot unavailable";
        return result;
    }
    size_t written =
        hu_doctor_check_outbound_stats_render_json(&snap, s_detail_buf, sizeof(s_detail_buf));
    if (written == 0) {
        /* JSON buffer too small (unlikely with 2KB) or snprintf
         * error. Surface as PASS with no detail so the check doesn't
         * regress dashboards on overflow. */
        result.detail_json = NULL;
        return result;
    }
    result.detail_json = s_detail_buf;
    return result;
}

const hu_doctor_check_t hu_doctor_check_outbound_stats = {
    .name = "outbound_stats",
    .description = "per-stage × per-verdict counters for the outbound pipeline",
    .run = outbound_stats_run,
    .fix = NULL,
    .user_data = NULL,
};
