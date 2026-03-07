#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "synthetic_harness.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

sc_error_t sc_synth_run_gateway(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                                sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics) {
    (void)gemini;
    SC_SYNTH_LOG("=== Gateway Tests ===");
    sc_synth_metrics_init(metrics);
    char tmpdir[256];
    pid_t pid = sc_synth_gateway_start(cfg->binary_path, cfg->gateway_port, tmpdir, sizeof(tmpdir));
    if (pid <= 0) {
        SC_SYNTH_LOG("gateway start failed");
        return SC_ERR_IO;
    }
    if (!sc_synth_gateway_wait(alloc, cfg->gateway_port, 15)) {
        sc_synth_gateway_stop(pid);
        sc_synth_gateway_cleanup(tmpdir);
        return SC_ERR_IO;
    }
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/health", cfg->gateway_port);
    sc_http_response_t resp = {0};
    double t0 = sc_synth_now_ms();
    sc_error_t err = sc_http_get(alloc, url, NULL, &resp);
    double lat = sc_synth_now_ms() - t0;
    long status = resp.status_code;
    sc_http_response_free(alloc, &resp);
    sc_synth_gateway_stop(pid);
    sc_synth_gateway_cleanup(tmpdir);
    sc_synth_verdict_t v = (err == SC_OK && status == 200) ? SC_SYNTH_PASS : SC_SYNTH_FAIL;
    sc_synth_metrics_record(alloc, metrics, lat, v);
    return SC_OK;
}

pid_t sc_synth_gateway_start(const char *binary, uint16_t port, char *tmpdir_out,
                             size_t tmpdir_len) {
    if (!binary || !tmpdir_out || tmpdir_len < 32)
        return -1;
    snprintf(tmpdir_out, tmpdir_len, "/tmp/seaclaw_synth_%u", (unsigned)port);
    mkdir(tmpdir_out, 0755);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        execl(binary, binary, "gateway", "--port", port_str, (char *)NULL);
        _exit(127);
    }
    return pid;
}

bool sc_synth_gateway_wait(sc_allocator_t *alloc, uint16_t port, int timeout_secs) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/health", port);
    for (int i = 0; i < timeout_secs * 10; i++) {
        sc_http_response_t resp = {0};
        if (sc_http_get(alloc, url, NULL, &resp) == SC_OK && resp.status_code == 200) {
            sc_http_response_free(alloc, &resp);
            return true;
        }
        sc_http_response_free(alloc, &resp);
        usleep(100000);
    }
    return false;
}

void sc_synth_gateway_stop(pid_t pid) {
    if (pid > 0)
        kill(pid, SIGTERM);
}

void sc_synth_gateway_cleanup(const char *tmpdir) {
    (void)tmpdir;
}
