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
 *   = ~1100 bytes total. Round to 2KB to leave headroom for future
 *     fields without re-tuning. */
#define HU_DOCTOR_OUTBOUND_STATS_JSON_BUF 2048

/* Per-call static buffer. The doctor result.detail_json is a
 * borrowed pointer; the check vtable owns the buffer. Single-threaded
 * use is fine because doctor runs checks sequentially. */
static char s_detail_buf[HU_DOCTOR_OUTBOUND_STATS_JSON_BUF];

/* Verdict-kind names in stable wire order. Matches the integer
 * values of hu_outbound_verdict_kind_t (SEND=0..REJECT=3). */
static const char *const VERDICT_FIELD_NAMES[HU_OUTBOUND_STATS_VERDICT_COUNT] = {
    "send",
    "rewrite",
    "regenerate",
    "reject",
};

size_t hu_doctor_check_outbound_stats_render_json(
    const struct hu_outbound_stats_snapshot *snap, char *buf, size_t cap) {
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
                     s == 0 ? "" : ",", stage_name,
                     (unsigned long long)snap->counts[s][0],
                     (unsigned long long)snap->counts[s][1],
                     (unsigned long long)snap->counts[s][2],
                     (unsigned long long)snap->counts[s][3]);
        if (n < 0 || (size_t)n >= cap - off)
            return 0;
        off += (size_t)n;
    }

    n = snprintf(buf + off, cap - off,
                 "],\"total_send\":%llu,\"total_rewrite\":%llu,"
                 "\"total_regenerate\":%llu,\"total_reject\":%llu}",
                 (unsigned long long)totals[0], (unsigned long long)totals[1],
                 (unsigned long long)totals[2], (unsigned long long)totals[3]);
    if (n < 0 || (size_t)n >= cap - off)
        return 0;
    off += (size_t)n;
    (void)VERDICT_FIELD_NAMES; /* reserved for a future variant that emits
                                * verdict_kind => count maps */
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
    size_t written = hu_doctor_check_outbound_stats_render_json(
        &snap, s_detail_buf, sizeof(s_detail_buf));
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
