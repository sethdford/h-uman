#include "human/core/http.h"
#include "synthetic_harness.h"
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static bool write_tmp_config(const hu_synth_config_t *cfg, char *tmpdir, size_t tmpdir_len) {
    snprintf(tmpdir, tmpdir_len, "/tmp/hu_synth_XXXXXX");
    if (!mkdtemp(tmpdir))
        return false;
    char scdir[512];
    snprintf(scdir, sizeof(scdir), "%s/.human", tmpdir);
    mkdir(scdir, 0755);
    char cfgpath[512];
    snprintf(cfgpath, sizeof(cfgpath), "%s/.human/config.json", tmpdir);
    FILE *f = fopen(cfgpath, "w");
    if (!f)
        return false;
    fprintf(f,
            "{\n"
            "  \"default_provider\": \"gemini\",\n"
            "  \"default_model\": \"%s\",\n"
            "  \"providers\": [{\"name\": \"gemini\", \"api_key\": \"%s\"}],\n"
            "  \"gateway\": {\"port\": %u}\n"
            "}",
            cfg->gemini_model, cfg->gemini_api_key ? cfg->gemini_api_key : "", cfg->gateway_port);
    fclose(f);
    return true;
}

pid_t hu_synth_gateway_start(const hu_synth_config_t *cfg, char *tmpdir_out, size_t tmpdir_len) {
    if (!write_tmp_config(cfg, tmpdir_out, tmpdir_len)) {
        HU_SYNTH_LOG("failed to write temp gateway config");
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", tmpdir_out, 1);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl(cfg->binary_path, cfg->binary_path, "gateway", (char *)NULL);
        _exit(127);
    }
    return pid;
}

void hu_synth_gateway_cleanup(const char *tmpdir) {
    if (!tmpdir || !tmpdir[0])
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.human/config.json", tmpdir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.human", tmpdir);
    rmdir(path);
    rmdir(tmpdir);
}

bool hu_synth_gateway_wait(hu_allocator_t *alloc, uint16_t port, int timeout_secs) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/health", port);
    for (int i = 0; i < timeout_secs * 10; i++) {
        hu_http_response_t r = {0};
        hu_error_t e = hu_http_get(alloc, url, NULL, &r);
        if (e == HU_OK && r.status_code == 200) {
            hu_http_response_free(alloc, &r);
            return true;
        }
        hu_http_response_free(alloc, &r);
        usleep(100000);
    }
    return false;
}

void hu_synth_gateway_stop(pid_t pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    int s = 0;
    waitpid(pid, &s, 0);
}

static void print_usage(const char *p) {
    printf("Usage: %s [OPTIONS]\nGemini-driven synthetic pressure tests for human.\n\n", p);
    printf("  --binary PATH         Path to human binary (default: ./human)\n");
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
    hu_synth_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.binary_path = "./human";
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
    hu_allocator_t alloc = hu_system_allocator();
    HU_SYNTH_LOG("=== human Synthetic Pressure Tests ===");
    HU_SYNTH_LOG("binary: %s  model: %s  port: %u", cfg.binary_path, cfg.gemini_model,
                 cfg.gateway_port);
    if (cfg.regression_dir)
        HU_SYNTH_LOG("regression dir: %s", cfg.regression_dir);

    if (cfg.replay_mode) {
        if (!cfg.replay_dir) {
            fprintf(stderr, "error: --replay requires a directory\n");
            return 1;
        }
        hu_synth_test_case_t *tests = NULL;
        size_t count = 0;
        hu_error_t rerr = hu_synth_regression_load(&alloc, cfg.replay_dir, &tests, &count);
        if (rerr != HU_OK) {
            fprintf(stderr, "failed to load regressions from %s: %s\n", cfg.replay_dir,
                    hu_error_string(rerr));
            return 1;
        }
        HU_SYNTH_LOG("loaded %zu saved test cases from %s", count, cfg.replay_dir);
        for (size_t i = 0; i < count; i++) {
            hu_synth_test_case_t *tc = &tests[i];
            printf("  [%zu] %-12s %-30s %s\n", i + 1, tc->category ? tc->category : "?",
                   tc->name ? tc->name : "?", tc->verdict_reason ? tc->verdict_reason : "");
            if (tc->input_json && cfg.verbose)
                printf("       input: %s\n", tc->input_json);
        }
        HU_SYNTH_LOG("replay complete: %zu cases", count);
        hu_synth_regression_free_tests(&alloc, tests, count);
        return 0;
    }

    if (!cfg.gemini_api_key) {
        fprintf(stderr, "error: GEMINI_API_KEY not set\n");
        return 1;
    }
    hu_synth_gemini_ctx_t *gemini = NULL;
    hu_error_t err = hu_synth_gemini_init(&alloc, cfg.gemini_api_key, cfg.gemini_model, &gemini);
    if (err != HU_OK) {
        fprintf(stderr, "Gemini init failed: %s\n", hu_error_string(err));
        return 1;
    }

    bool any =
        cfg.cli_only || cfg.gateway_only || cfg.ws_only || cfg.agent_only || cfg.pressure_only;
    hu_synth_metrics_t cm = {0}, gm = {0}, wm = {0}, am = {0}, pm = {0};

    if (!any || cfg.cli_only)
        hu_synth_run_cli(&alloc, &cfg, gemini, &cm);

    bool need_gw = (!any || cfg.gateway_only || cfg.ws_only || cfg.agent_only || cfg.pressure_only);
    pid_t gw = -1;
    char gw_tmpdir[256] = {0};

    if (need_gw) {
        HU_SYNTH_LOG("starting gateway on port %u with provider gemini/%s...", cfg.gateway_port,
                     cfg.gemini_model);
        gw = hu_synth_gateway_start(&cfg, gw_tmpdir, sizeof(gw_tmpdir));
        if (gw < 0 || !hu_synth_gateway_wait(&alloc, cfg.gateway_port, 15)) {
            HU_SYNTH_LOG("gateway failed to start");
            if (gw > 0)
                hu_synth_gateway_stop(gw);
            hu_synth_gateway_cleanup(gw_tmpdir);
            hu_synth_gemini_deinit(&alloc, gemini);
            return 1;
        }
        HU_SYNTH_LOG("gateway ready (pid=%d)", gw);
    }

    if (!any || cfg.gateway_only)
        hu_synth_run_gateway(&alloc, &cfg, gemini, &gm);
    if (!any || cfg.ws_only)
        hu_synth_run_ws(&alloc, &cfg, gemini, &wm);
    if (!any || cfg.agent_only)
        hu_synth_run_agent(&alloc, &cfg, gemini, &am);
    if (!any || cfg.pressure_only)
        hu_synth_run_pressure(&alloc, &cfg, gemini, &pm);

    if (gw > 0) {
        HU_SYNTH_LOG("stopping gateway...");
        hu_synth_gateway_stop(gw);
        hu_synth_gateway_cleanup(gw_tmpdir);
    }

    HU_SYNTH_LOG("========== Final Report ==========");
    if (cm.total)
        hu_synth_report_category("CLI", &cm);
    if (gm.total)
        hu_synth_report_category("Gateway", &gm);
    if (wm.total)
        hu_synth_report_category("WebSocket", &wm);
    if (am.total)
        hu_synth_report_category("Agent", &am);
    if (pm.total)
        hu_synth_report_category("Pressure", &pm);

    int tf = cm.failed + gm.failed + wm.failed + am.failed + pm.failed;
    int te = cm.errors + gm.errors + wm.errors + am.errors + pm.errors;
    int tp = cm.passed + gm.passed + wm.passed + am.passed + pm.passed;
    int ta = cm.total + gm.total + wm.total + am.total + pm.total;
    HU_SYNTH_LOG("Total: %d/%d passed, %d failed, %d errors", tp, ta, tf, te);

    hu_synth_metrics_free(&alloc, &cm);
    hu_synth_metrics_free(&alloc, &gm);
    hu_synth_metrics_free(&alloc, &wm);
    hu_synth_metrics_free(&alloc, &am);
    hu_synth_metrics_free(&alloc, &pm);
    hu_synth_gemini_deinit(&alloc, gemini);
    return (tf > 0 || te > 0) ? 1 : 0;
}
