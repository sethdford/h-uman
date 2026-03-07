#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "synthetic_harness.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void sc_synth_test_case_free(sc_allocator_t *alloc, sc_synth_test_case_t *tc) {
    if (!tc)
        return;
    if (tc->name)
        sc_synth_strfree(alloc, tc->name, strlen(tc->name));
    if (tc->category)
        sc_synth_strfree(alloc, tc->category, strlen(tc->category));
    if (tc->input_json)
        alloc->free(alloc->ctx, tc->input_json, strlen(tc->input_json) + 1);
    if (tc->expected_json)
        alloc->free(alloc->ctx, tc->expected_json, strlen(tc->expected_json) + 1);
    if (tc->actual_output)
        sc_synth_strfree(alloc, tc->actual_output, strlen(tc->actual_output));
    if (tc->verdict_reason)
        sc_synth_strfree(alloc, tc->verdict_reason, strlen(tc->verdict_reason));
    memset(tc, 0, sizeof(*tc));
}

void sc_synth_metrics_init(sc_synth_metrics_t *m) {
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
}

void sc_synth_metrics_record(sc_allocator_t *alloc, sc_synth_metrics_t *m, double latency_ms,
                             sc_synth_verdict_t v) {
    if (!m)
        return;
    if (m->latency_count >= m->latency_cap) {
        size_t nc = m->latency_cap ? m->latency_cap * 2 : 64;
        double *narr = (double *)alloc->alloc(alloc->ctx, nc * sizeof(double));
        if (!narr)
            return;
        memcpy(narr, m->latencies, m->latency_count * sizeof(double));
        if (m->latencies)
            alloc->free(alloc->ctx, m->latencies, m->latency_cap * sizeof(double));
        m->latencies = narr;
        m->latency_cap = nc;
    }
    m->latencies[m->latency_count++] = latency_ms;
    m->total++;
    switch (v) {
    case SC_SYNTH_PASS:
        m->passed++;
        break;
    case SC_SYNTH_FAIL:
        m->failed++;
        break;
    case SC_SYNTH_ERROR:
        m->errors++;
        break;
    case SC_SYNTH_SKIP:
        m->skipped++;
        break;
    default:
        break;
    }
}

void sc_synth_metrics_free(sc_allocator_t *alloc, sc_synth_metrics_t *m) {
    if (!m)
        return;
    if (m->latencies)
        alloc->free(alloc->ctx, m->latencies, m->latency_cap * sizeof(double));
    m->latencies = NULL;
    m->latency_count = m->latency_cap = 0;
}

double sc_synth_metrics_avg(const sc_synth_metrics_t *m) {
    if (!m || m->latency_count == 0)
        return 0;
    double s = 0;
    for (size_t i = 0; i < m->latency_count; i++)
        s += m->latencies[i];
    return s / (double)m->latency_count;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

double sc_synth_metrics_percentile(const sc_synth_metrics_t *m, double pct) {
    if (!m || m->latency_count == 0)
        return 0;
    double *copy = (double *)malloc(m->latency_count * sizeof(double));
    if (!copy)
        return 0;
    memcpy(copy, m->latencies, m->latency_count * sizeof(double));
    qsort(copy, m->latency_count, sizeof(double), cmp_double);
    size_t idx = (size_t)(pct / 100.0 * (double)m->latency_count);
    if (idx >= m->latency_count)
        idx = m->latency_count - 1;
    double r = copy[idx];
    free(copy);
    return r;
}

double sc_synth_metrics_max(const sc_synth_metrics_t *m) {
    if (!m || m->latency_count == 0)
        return 0;
    double mx = m->latencies[0];
    for (size_t i = 1; i < m->latency_count; i++)
        if (m->latencies[i] > mx)
            mx = m->latencies[i];
    return mx;
}

void sc_synth_report_category(const char *name, const sc_synth_metrics_t *m) {
    if (!name || !m)
        return;
    double avg = sc_synth_metrics_avg(m);
    double p95 = sc_synth_metrics_percentile(m, 95);
    SC_SYNTH_LOG("%s: total=%d passed=%d failed=%d errors=%d skipped=%d avg=%.1fms p95=%.1fms",
                 name, m->total, m->passed, m->failed, m->errors, m->skipped, avg, p95);
}

void sc_synth_report_final(void) {
    SC_SYNTH_LOG("=== Synthetic run complete ===");
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --binary PATH "
            "[--cli-only|--gateway-only|--ws-only|--agent-only|--pressure-only] [--count N] "
            "[--port P] [--gemini-key KEY] [--gemini-model MODEL]\n",
            prog);
}

int main(int argc, char **argv) {
    sc_synth_config_t cfg = {0};
    cfg.gateway_port = 8765;
    cfg.concurrency = 4;
    cfg.duration_secs = 10;
    cfg.tests_per_category = 5;
    static struct option opts[] = {
        {"binary", required_argument, 0, 'b'},     {"cli-only", no_argument, 0, 'c'},
        {"gateway-only", no_argument, 0, 'g'},     {"ws-only", no_argument, 0, 'w'},
        {"agent-only", no_argument, 0, 'a'},       {"pressure-only", no_argument, 0, 'p'},
        {"count", required_argument, 0, 'n'},      {"port", required_argument, 0, 'P'},
        {"gemini-key", required_argument, 0, 'k'}, {"gemini-model", required_argument, 0, 'm'},
        {"verbose", no_argument, 0, 'v'},          {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "b:cgwapn:P:k:m:v", opts, NULL)) != -1) {
        switch (c) {
        case 'b':
            cfg.binary_path = optarg;
            break;
        case 'c':
            cfg.cli_only = true;
            break;
        case 'g':
            cfg.gateway_only = true;
            break;
        case 'w':
            cfg.ws_only = true;
            break;
        case 'a':
            cfg.agent_only = true;
            break;
        case 'p':
            cfg.pressure_only = true;
            break;
        case 'n':
            cfg.tests_per_category = atoi(optarg);
            break;
        case 'P':
            cfg.gateway_port = (uint16_t)atoi(optarg);
            break;
        case 'k':
            cfg.gemini_api_key = optarg;
            break;
        case 'm':
            cfg.gemini_model = optarg ? optarg : "gemini-2.5-flash";
            break;
        case 'v':
            cfg.verbose = true;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (!cfg.binary_path) {
        usage(argv[0]);
        return 1;
    }
    if (!cfg.cli_only && !cfg.gateway_only && !cfg.ws_only && !cfg.agent_only && !cfg.pressure_only)
        cfg.cli_only = true;

    sc_allocator_t *alloc = sc_allocator_default();
    sc_synth_metrics_t metrics = {0};
    sc_synth_gemini_ctx_t *gemini = NULL;
    if (cfg.gemini_api_key && cfg.gemini_model) {
        if (sc_synth_gemini_init(alloc, cfg.gemini_api_key, cfg.gemini_model, &gemini) != SC_OK)
            gemini = NULL;
    }

    sc_error_t err = SC_OK;
    if (cfg.cli_only)
        err = sc_synth_run_cli(alloc, &cfg, gemini, &metrics);
    else if (cfg.gateway_only)
        err = sc_synth_run_gateway(alloc, &cfg, gemini, &metrics);
    else if (cfg.ws_only)
        err = sc_synth_run_ws(alloc, &cfg, gemini, &metrics);
    else if (cfg.agent_only)
        err = sc_synth_run_agent(alloc, &cfg, gemini, &metrics);
    else if (cfg.pressure_only)
        err = sc_synth_run_pressure(alloc, &cfg, gemini, &metrics);

    if (gemini)
        sc_synth_gemini_deinit(alloc, gemini);
    sc_synth_report_category("synthetic", &metrics);
    sc_synth_report_final();
    sc_synth_metrics_free(alloc, &metrics);
    return err == SC_OK ? 0 : 1;
}
