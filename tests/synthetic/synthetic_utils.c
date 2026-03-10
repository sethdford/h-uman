#include "synthetic_harness.h"

void hu_synth_test_case_free(hu_allocator_t *alloc, hu_synth_test_case_t *tc) {
    if (!tc)
        return;
    if (tc->name)
        hu_synth_strfree(alloc, tc->name, strlen(tc->name));
    if (tc->category)
        hu_synth_strfree(alloc, tc->category, strlen(tc->category));
    if (tc->actual_output)
        hu_synth_strfree(alloc, tc->actual_output, strlen(tc->actual_output));
    if (tc->verdict_reason)
        hu_synth_strfree(alloc, tc->verdict_reason, strlen(tc->verdict_reason));
    if (tc->input_json)
        alloc->free(alloc->ctx, tc->input_json, strlen(tc->input_json));
    if (tc->expected_json)
        alloc->free(alloc->ctx, tc->expected_json, strlen(tc->expected_json));
    memset(tc, 0, sizeof(*tc));
}

void hu_synth_metrics_init(hu_synth_metrics_t *m) {
    memset(m, 0, sizeof(*m));
}

void hu_synth_metrics_record(hu_allocator_t *alloc, hu_synth_metrics_t *m, double lat,
                             hu_synth_verdict_t v) {
    m->total++;
    switch (v) {
    case HU_SYNTH_PASS:
        m->passed++;
        break;
    case HU_SYNTH_FAIL:
        m->failed++;
        break;
    case HU_SYNTH_ERROR:
        m->errors++;
        break;
    case HU_SYNTH_SKIP:
        m->skipped++;
        break;
    }
    if (m->latency_count >= m->latency_cap) {
        size_t nc = m->latency_cap == 0 ? 64 : m->latency_cap * 2;
        double *nl = (double *)alloc->alloc(alloc->ctx, nc * sizeof(double));
        if (!nl)
            return;
        if (m->latencies) {
            memcpy(nl, m->latencies, m->latency_count * sizeof(double));
            alloc->free(alloc->ctx, m->latencies, m->latency_cap * sizeof(double));
        }
        m->latencies = nl;
        m->latency_cap = nc;
    }
    m->latencies[m->latency_count++] = lat;
}

void hu_synth_metrics_free(hu_allocator_t *alloc, hu_synth_metrics_t *m) {
    if (m->latencies)
        alloc->free(alloc->ctx, m->latencies, m->latency_cap * sizeof(double));
    memset(m, 0, sizeof(*m));
}

static int dbl_cmp(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

double hu_synth_metrics_avg(const hu_synth_metrics_t *m) {
    if (!m->latency_count)
        return 0;
    double s = 0;
    for (size_t i = 0; i < m->latency_count; i++)
        s += m->latencies[i];
    return s / (double)m->latency_count;
}

double hu_synth_metrics_percentile(const hu_synth_metrics_t *m, double pct) {
    if (!m->latency_count)
        return 0;
    double *s = (double *)malloc(m->latency_count * sizeof(double));
    if (!s)
        return 0;
    memcpy(s, m->latencies, m->latency_count * sizeof(double));
    qsort(s, m->latency_count, sizeof(double), dbl_cmp);
    size_t idx = (size_t)((pct / 100.0) * (double)(m->latency_count - 1));
    if (idx >= m->latency_count)
        idx = m->latency_count - 1;
    double v = s[idx];
    free(s);
    return v;
}

double hu_synth_metrics_max(const hu_synth_metrics_t *m) {
    if (!m->latency_count)
        return 0;
    double mx = m->latencies[0];
    for (size_t i = 1; i < m->latency_count; i++)
        if (m->latencies[i] > mx)
            mx = m->latencies[i];
    return mx;
}

void hu_synth_report_category(const char *name, const hu_synth_metrics_t *m) {
    printf("[synth] %-12s %d/%d passed", name, m->passed, m->total);
    if (m->failed > 0)
        printf(", %d FAILED", m->failed);
    if (m->errors > 0)
        printf(", %d errors", m->errors);
    if (m->latency_count > 0)
        printf("  (avg %.1fms, p99 %.1fms)", hu_synth_metrics_avg(m),
               hu_synth_metrics_percentile(m, 99));
    printf("\n");
}

void hu_synth_report_final(void) {
    printf("\n");
}
