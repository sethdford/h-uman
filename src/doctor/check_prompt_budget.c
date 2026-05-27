/* src/doctor/check_prompt_budget.c
 *
 * Sprint 55 B3 Task 5 — prompt budget doctor check implementation.
 * Extended 2026-05-26 (docs/plans/2026-05-25-doctor-prompt-budget-initiative/)
 * to enrich detail_json with live observation_count + top fields when the
 * daemon's snapshot file is present at ~/.human/prompt_budget.snapshot.json.
 *
 * Behavior matrix:
 *   enabled=false → NA, detail_json = config-only (snapshot is irrelevant)
 *   enabled=true, snapshot missing/ENOENT → PASS but detail_json carries
 *       "snapshot_present":false (operator knows config is on but file
 *       hasn't materialized yet — daemon just started, or quiet path).
 *   enabled=true, snapshot fresh (mtime within 120s) → PASS, detail_json
 *       carries observation_count + samples_total + top fields.
 *   enabled=true, snapshot stale (mtime >120s) → FAIL with reason naming
 *       the staleness — daemon may have stopped flushing.
 *   enabled=true, snapshot parse error → FAIL with parse-error reason.
 */

#include "human/doctor/check_prompt_budget.h"

#include "human/agent/prompt_budget.h"
#include "human/config.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Static buffers for borrowed strings per the check.h contract. The
 * detail buffer is sized to fit observation_count + a Top-N (5) field
 * array; each field row is ~80 chars so 2 KB comfortably contains it. */
static char s_reason_buf[512];
static char s_detail_json_buf[2048];

/* Staleness threshold — 2x the daemon's 60s flush cadence. Tighter than
 * 2x would alarm on routine timing skew; looser would hide a wedged
 * daemon. Per design.md D8 + the verifier check's 5-minute precedent. */
#define HU_DOCTOR_PB_STALE_AFTER_SECS 120

/* Top-N fields to surface in detail_json. Chosen for fit in the static
 * buffer; the snapshot file always carries the full HU_PROMPT_FIELD_COUNT
 * for an operator who wants more (`cat ~/.human/prompt_budget.snapshot.json`). */
#define HU_DOCTOR_PB_TOP_FIELDS 5

/* qsort comparator: descending by mean_bytes. */
static int cmp_field_by_mean_desc(const void *a, const void *b) {
    const hu_prompt_budget_field_stat_ext_t *fa = (const hu_prompt_budget_field_stat_ext_t *)a;
    const hu_prompt_budget_field_stat_ext_t *fb = (const hu_prompt_budget_field_stat_ext_t *)b;
    if (fb->mean_bytes > fa->mean_bytes)
        return 1;
    if (fb->mean_bytes < fa->mean_bytes)
        return -1;
    return 0;
}

/* ── helpers ─────────────────────────────────────────────────────── */

/* Build the config-only detail (no snapshot file present). */
static void build_config_only_detail(bool enabled, bool snapshot_attempted) {
    snprintf(s_detail_json_buf, sizeof(s_detail_json_buf),
             "{\"enabled\":%s,"
             "\"snapshot_present\":false,"
             "\"snapshot_attempted\":%s,"
             "\"observation_count\":null,"
             "\"dead_field_count\":null,"
             "\"note\":\"doctor read no snapshot file — daemon may not have flushed yet, "
             "or HU_IS_TEST path override empty. Config state only.\"}",
             enabled ? "true" : "false", snapshot_attempted ? "true" : "false");
}

/* Build the enriched detail using a successfully-loaded snapshot. */
static void build_enriched_detail(bool enabled, const hu_prompt_budget_snapshot_load_t *load,
                                  int64_t now_unix) {
    int64_t age = (load->mtime_unix > 0) ? (now_unix - load->mtime_unix) : 0;
    /* Sort a private copy of the fields so we can pick top-N. */
    hu_prompt_budget_field_stat_ext_t sorted[64];
    size_t n = load->field_count < 64 ? load->field_count : 64;
    for (size_t i = 0; i < n; i++)
        sorted[i] = load->fields[i];
    if (n > 1)
        qsort(sorted, n, sizeof(sorted[0]), cmp_field_by_mean_desc);
    size_t top = n < HU_DOCTOR_PB_TOP_FIELDS ? n : HU_DOCTOR_PB_TOP_FIELDS;

    size_t off = 0;
    int w = snprintf(s_detail_json_buf, sizeof(s_detail_json_buf),
                     "{\"enabled\":%s,"
                     "\"snapshot_present\":true,"
                     "\"snapshot_age_seconds\":%lld,"
                     "\"observation_count\":%llu,"
                     "\"field_count\":%zu,"
                     "\"top_fields\":[",
                     enabled ? "true" : "false", (long long)age,
                     (unsigned long long)load->observation_count, load->field_count);
    if (w < 0)
        return;
    off = (size_t)w;
    for (size_t i = 0; i < top && off < sizeof(s_detail_json_buf); i++) {
        const char *name = sorted[i].name ? sorted[i].name : "?";
        int rw = snprintf(s_detail_json_buf + off, sizeof(s_detail_json_buf) - off,
                          "%s{\"name\":\"%s\",\"mean_bytes\":%llu,"
                          "\"samples\":%llu,\"non_empty_count\":%llu}",
                          i == 0 ? "" : ",", name, (unsigned long long)sorted[i].mean_bytes,
                          (unsigned long long)sorted[i].samples,
                          (unsigned long long)sorted[i].non_empty_count);
        if (rw < 0)
            break;
        off += (size_t)rw;
    }
    if (off < sizeof(s_detail_json_buf))
        snprintf(s_detail_json_buf + off, sizeof(s_detail_json_buf) - off, "]}");
}

/* ── vtable runner ────────────────────────────────────────────────── */

static hu_doctor_check_result_t check_prompt_budget_run(hu_doctor_check_t *self, void *ctx) {
    (void)self;

    /* ctx is hu_doctor_check_prompt_budget_ctx_t. NULL ctx OR NULL cfg → NA. */
    const hu_doctor_check_prompt_budget_ctx_t *pctx =
        (const hu_doctor_check_prompt_budget_ctx_t *)ctx;
    const struct hu_config *cfg = pctx ? pctx->cfg : NULL;

    if (!cfg) {
        return (hu_doctor_check_result_t){HU_DOCTOR_NA,
                                          "no config provided to doctor — prompt_budget check "
                                          "skipped",
                                          NULL};
    }

    bool enabled = cfg->prompt_budget.enabled;

    if (!enabled) {
        /* Config-only path: snapshot is irrelevant when observation is
         * off. Preserves the pre-extension contract (NA + config-only
         * detail). */
        build_config_only_detail(/*enabled=*/false, /*snapshot_attempted=*/false);
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget.enabled=false in config.json — observer + trim gate "
                 "inactive. Set true to activate.");
        return (hu_doctor_check_result_t){HU_DOCTOR_NA, s_reason_buf, s_detail_json_buf};
    }

    /* enabled=true → try to read the daemon's snapshot file via the
     * standard system allocator. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_snapshot_load_t load;
    hu_error_t lerr = hu_prompt_budget_load_snapshot(&alloc, &load);

    if (lerr == HU_ERR_NOT_FOUND) {
        build_config_only_detail(/*enabled=*/true, /*snapshot_attempted=*/true);
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget.enabled=true; no snapshot at "
                 "~/.human/prompt_budget.snapshot.json yet (daemon may be starting).");
        return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason_buf, s_detail_json_buf};
    }
    if (lerr == HU_ERR_PARSE) {
        build_config_only_detail(/*enabled=*/true, /*snapshot_attempted=*/true);
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget snapshot malformed — re-flushing on next daemon tick "
                 "should fix. detail_json reports config state only.");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_buf, s_detail_json_buf};
    }
    if (lerr != HU_OK) {
        build_config_only_detail(/*enabled=*/true, /*snapshot_attempted=*/true);
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget snapshot unreadable (filesystem error). detail_json "
                 "reports config state only.");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_buf, s_detail_json_buf};
    }

    int64_t now_unix = (int64_t)time(NULL);
    int64_t age_s = load.mtime_unix > 0 ? (now_unix - load.mtime_unix) : 0;
    bool stale = age_s > HU_DOCTOR_PB_STALE_AFTER_SECS;

    build_enriched_detail(/*enabled=*/true, &load, now_unix);

    if (stale) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget snapshot is %lld seconds old (>%d) — daemon may not be "
                 "flushing. Last %llu observations recorded.",
                 (long long)age_s, HU_DOCTOR_PB_STALE_AFTER_SECS,
                 (unsigned long long)load.observation_count);
        hu_prompt_budget_snapshot_load_free(&load);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_buf, s_detail_json_buf};
    }

    if (load.observation_count == 0) {
        snprintf(s_reason_buf, sizeof(s_reason_buf),
                 "prompt_budget.enabled=true and snapshot is fresh, but "
                 "observation_count=0 — appender may not be invoked. Run a turn to verify.");
        hu_prompt_budget_snapshot_load_free(&load);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_buf, s_detail_json_buf};
    }

    snprintf(s_reason_buf, sizeof(s_reason_buf),
             "prompt_budget.enabled=true; snapshot fresh (%lld s old) with %llu observations "
             "across %zu fields.",
             (long long)age_s, (unsigned long long)load.observation_count, load.field_count);
    hu_prompt_budget_snapshot_load_free(&load);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason_buf, s_detail_json_buf};
}

/* ── Vtable entry ─────────────────────────────────────────────────── */

hu_doctor_check_t hu_doctor_check_prompt_budget = {
    .name = "prompt_budget",
    .description = "Reports prompt-budget config state (observer + trim gate)",
    .run = check_prompt_budget_run,
    .fix = NULL, /* No autofix — operator edits config.json */
    .user_data = NULL,
};
