#include "seaclaw/core/http.h"
#include "synthetic_harness.h"
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void sc_synth_test_case_free(sc_allocator_t *alloc, sc_synth_test_case_t *tc) {
    if (!tc)
        return;
    if (tc->name)
        sc_synth_strfree(alloc, tc->name, strlen(tc->name));
    if (tc->category)
        sc_synth_strfree(alloc, tc->category, strlen(tc->category));
    if (tc->actual_output)
        sc_synth_strfree(alloc, tc->actual_output, strlen(tc->actual_output));
    if (tc->verdict_reason)
        sc_synth_strfree(alloc, tc->verdict_reason, strlen(tc->verdict_reason));
    if (tc->input_json)
        alloc->free(alloc->ctx, tc->input_json, strlen(tc->input_json));
    if (tc->expected_json)
        alloc->free(alloc->ctx, tc->expected_json, strlen(tc->expected_json));
    memset(tc, 0, sizeof(*tc));
}

void sc_synth_metrics_init(sc_synth_metrics_t *m) {
    memset(m, 0, sizeof(*m));
}

void sc_synth_metrics_record(sc_allocator_t *alloc, sc_synth_metrics_t *m, double lat,
                             sc_synth_verdict_t v) {
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

void sc_synth_metrics_free(sc_allocator_t *alloc, sc_synth_metrics_t *m) {
    if (m->latencies)
        alloc->free(alloc->ctx, m->latencies, m->latency_cap * sizeof(double));
    memset(m, 0, sizeof(*m));
}

static int dbl_cmp(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

double sc_synth_metrics_avg(const sc_synth_metrics_t *m) {
    if (!m->latency_count)
        return 0;
    double s = 0;
    for (size_t i = 0; i < m->latency_count; i++)
        s += m->latencies[i];
    return s / (double)m->latency_count;
}

double sc_synth_metrics_percentile(const sc_synth_metrics_t *m, double pct) {
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

double sc_synth_metrics_max(const sc_synth_metrics_t *m) {
    if (!m->latency_count)
        return 0;
    double mx = m->latencies[0];
    for (size_t i = 1; i < m->latency_count; i++)
        if (m->latencies[i] > mx)
            mx = m->latencies[i];
    return mx;
}

void sc_synth_report_category(const char *name, const sc_synth_metrics_t *m) {
    printf("[synth] %-12s %d/%d passed", name, m->passed, m->total);
    if (m->failed > 0)
        printf(", %d FAILED", m->failed);
    if (m->errors > 0)
        printf(", %d errors", m->errors);
    if (m->latency_count > 0)
        printf("  (avg %.1fms, p99 %.1fms)", sc_synth_metrics_avg(m),
               sc_synth_metrics_percentile(m, 99));
    printf("\n");
}

void sc_synth_report_final(void) {
    printf("\n");
}

static bool write_tmp_config(uint16_t port, char *tmpdir, size_t tmpdir_len) {
    snprintf(tmpdir, tmpdir_len, "/tmp/sc_synth_XXXXXX");
    if (!mkdtemp(tmpdir))
        return false;
    char scdir[512];
    snprintf(scdir, sizeof(scdir), "%s/.seaclaw", tmpdir);
    mkdir(scdir, 0755);
    char cfgpath[512];
    snprintf(cfgpath, sizeof(cfgpath), "%s/.seaclaw/config.json", tmpdir);
    FILE *f = fopen(cfgpath, "w");
    if (!f)
        return false;
    fprintf(f, "{\"gateway\":{\"port\":%u}}", port);
    fclose(f);
    return true;
}

pid_t sc_synth_gateway_start(const char *binary, uint16_t port, char *tmpdir_out,
                             size_t tmpdir_len) {
    if (!write_tmp_config(port, tmpdir_out, tmpdir_len)) {
        SC_SYNTH_LOG("failed to write temp gateway config");
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", tmpdir_out, 1);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl(binary, binary, "gateway", (char *)NULL);
        _exit(127);
    }
    return pid;
}

void sc_synth_gateway_cleanup(const char *tmpdir) {
    if (!tmpdir || !tmpdir[0])
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.seaclaw/config.json", tmpdir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.seaclaw", tmpdir);
    rmdir(path);
    rmdir(tmpdir);
}

bool sc_synth_gateway_wait(sc_allocator_t *alloc, uint16_t port, int timeout_secs) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/health", port);
    for (int i = 0; i < timeout_secs * 10; i++) {
        sc_http_response_t r = {0};
        sc_error_t e = sc_http_get(alloc, url, NULL, &r);
        if (e == SC_OK && r.status_code == 200) {
            sc_http_response_free(alloc, &r);
            return true;
        }
        sc_http_response_free(alloc, &r);
        usleep(100000);
    }
    return false;
}

void sc_synth_gateway_stop(pid_t pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    int s = 0;
    waitpid(pid, &s, 0);
}

static void print_usage(const char *p) {
    printf("Usage: %s [OPTIONS]\nGemini-driven synthetic pressure tests for seaclaw.\n\n", p);
    printf("  --binary PATH         Path to seaclaw binary (default: ./seaclaw)\n");
    printf("  --model MODEL         Gemini model (default: gemini-2.5-flash)\n");
    printf("  --port PORT           Gateway port (default: 3199)\n");
    printf("  --count N             Tests per category (default: 20)\n");
    printf("  --concurrency N       Pressure workers (default: 4)\n");
    printf("  --duration SECS       Pressure duration (default: 10)\n");
    printf("  --regression-dir DIR  Save failures to DIR for replay\n");
    printf("  --replay DIR          Replay previously saved failures from DIR\n");
    printf("  --cli-only            Only CLI tests\n");
    printf("  --gateway-only        Only gateway tests\n");
    printf("  --ws-only             Only WS tests\n");
    printf("  --agent-only          Only agent tests\n");
    printf("  --pressure-only       Only pressure tests\n");
    printf("  --verbose             Verbose output\n");
    printf("  --help                Show help\n");
    printf("\nEnv: GEMINI_API_KEY (required)\n");
}

int main(int argc, char **argv) {
    sc_synth_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.binary_path = "./seaclaw";
    cfg.gemini_model = "gemini-2.5-flash";
    cfg.gateway_port = 3199;
    cfg.concurrency = 4;
    cfg.duration_secs = 10;
    cfg.tests_per_category = 20;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--binary") && i + 1 < argc)
            cfg.binary_path = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            cfg.gemini_model = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            cfg.gateway_port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc)
            cfg.tests_per_category = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--concurrency") && i + 1 < argc)
            cfg.concurrency = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            cfg.duration_secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--regression-dir") && i + 1 < argc)
            cfg.regression_dir = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) {
            cfg.replay_mode = true;
            cfg.replay_dir = argv[++i];
        } else if (!strcmp(argv[i], "--cli-only"))
            cfg.cli_only = true;
        else if (!strcmp(argv[i], "--gateway-only"))
            cfg.gateway_only = true;
        else if (!strcmp(argv[i], "--ws-only"))
            cfg.ws_only = true;
        else if (!strcmp(argv[i], "--agent-only"))
            cfg.agent_only = true;
        else if (!strcmp(argv[i], "--pressure-only"))
            cfg.pressure_only = true;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v"))
            cfg.verbose = true;
        else {
            fprintf(stderr, "Unknown: %s\n", argv[i]);
            return 1;
        }
    }

    cfg.gemini_api_key = getenv("GEMINI_API_KEY");
    sc_allocator_t alloc = sc_system_allocator();
    SC_SYNTH_LOG("=== seaclaw Synthetic Pressure Tests ===");
    SC_SYNTH_LOG("binary: %s  model: %s  port: %u", cfg.binary_path, cfg.gemini_model,
                 cfg.gateway_port);
    if (cfg.regression_dir)
        SC_SYNTH_LOG("regression dir: %s", cfg.regression_dir);

    if (cfg.replay_mode) {
        if (!cfg.replay_dir) {
            fprintf(stderr, "error: --replay requires a directory\n");
            return 1;
        }
        sc_synth_test_case_t *tests = NULL;
        size_t count = 0;
        sc_error_t rerr = sc_synth_regression_load(&alloc, cfg.replay_dir, &tests, &count);
        if (rerr != SC_OK) {
            fprintf(stderr, "failed to load regressions from %s: %s\n", cfg.replay_dir,
                    sc_error_string(rerr));
            return 1;
        }
        SC_SYNTH_LOG("loaded %zu saved test cases from %s", count, cfg.replay_dir);
        for (size_t i = 0; i < count; i++) {
            sc_synth_test_case_t *tc = &tests[i];
            printf("  [%zu] %-12s %-30s %s\n", i + 1, tc->category ? tc->category : "?",
                   tc->name ? tc->name : "?", tc->verdict_reason ? tc->verdict_reason : "");
            if (tc->input_json && cfg.verbose)
                printf("       input: %s\n", tc->input_json);
        }
        SC_SYNTH_LOG("replay complete: %zu cases", count);
        sc_synth_regression_free_tests(&alloc, tests, count);
        return 0;
    }

    if (!cfg.gemini_api_key) {
        fprintf(stderr, "error: GEMINI_API_KEY not set\n");
        return 1;
    }
    sc_synth_gemini_ctx_t *gemini = NULL;
    sc_error_t err = sc_synth_gemini_init(&alloc, cfg.gemini_api_key, cfg.gemini_model, &gemini);
    if (err != SC_OK) {
        fprintf(stderr, "Gemini init failed: %s\n", sc_error_string(err));
        return 1;
    }

    bool any =
        cfg.cli_only || cfg.gateway_only || cfg.ws_only || cfg.agent_only || cfg.pressure_only;
    sc_synth_metrics_t cm = {0}, gm = {0}, wm = {0}, am = {0}, pm = {0};

    if (!any || cfg.cli_only)
        sc_synth_run_cli(&alloc, &cfg, gemini, &cm);

    bool need_gw = (!any || cfg.gateway_only || cfg.ws_only || cfg.agent_only || cfg.pressure_only);
    pid_t gw = -1;
    char gw_tmpdir[256] = {0};

    if (need_gw) {
        SC_SYNTH_LOG("starting gateway on port %u...", cfg.gateway_port);
        gw =
            sc_synth_gateway_start(cfg.binary_path, cfg.gateway_port, gw_tmpdir, sizeof(gw_tmpdir));
        if (gw < 0 || !sc_synth_gateway_wait(&alloc, cfg.gateway_port, 15)) {
            SC_SYNTH_LOG("gateway failed to start");
            if (gw > 0)
                sc_synth_gateway_stop(gw);
            sc_synth_gateway_cleanup(gw_tmpdir);
            sc_synth_gemini_deinit(&alloc, gemini);
            return 1;
        }
        SC_SYNTH_LOG("gateway ready (pid=%d)", gw);
    }

    if (!any || cfg.gateway_only)
        sc_synth_run_gateway(&alloc, &cfg, gemini, &gm);
    if (!any || cfg.ws_only)
        sc_synth_run_ws(&alloc, &cfg, gemini, &wm);
    if (!any || cfg.agent_only)
        sc_synth_run_agent(&alloc, &cfg, gemini, &am);
    if (!any || cfg.pressure_only)
        sc_synth_run_pressure(&alloc, &cfg, gemini, &pm);

    if (gw > 0) {
        SC_SYNTH_LOG("stopping gateway...");
        sc_synth_gateway_stop(gw);
        sc_synth_gateway_cleanup(gw_tmpdir);
    }

    SC_SYNTH_LOG("========== Final Report ==========");
    if (cm.total)
        sc_synth_report_category("CLI", &cm);
    if (gm.total)
        sc_synth_report_category("Gateway", &gm);
    if (wm.total)
        sc_synth_report_category("WebSocket", &wm);
    if (am.total)
        sc_synth_report_category("Agent", &am);
    if (pm.total)
        sc_synth_report_category("Pressure", &pm);

    int tf = cm.failed + gm.failed + wm.failed + am.failed + pm.failed;
    int te = cm.errors + gm.errors + wm.errors + am.errors + pm.errors;
    int tp = cm.passed + gm.passed + wm.passed + am.passed + pm.passed;
    int ta = cm.total + gm.total + wm.total + am.total + pm.total;
    SC_SYNTH_LOG("Total: %d/%d passed, %d failed, %d errors", tp, ta, tf, te);

    sc_synth_metrics_free(&alloc, &cm);
    sc_synth_metrics_free(&alloc, &gm);
    sc_synth_metrics_free(&alloc, &wm);
    sc_synth_metrics_free(&alloc, &am);
    sc_synth_metrics_free(&alloc, &pm);
    sc_synth_gemini_deinit(&alloc, gemini);
    return (tf > 0 || te > 0) ? 1 : 0;
}
