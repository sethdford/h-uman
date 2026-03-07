#include "seaclaw/core/http.h"
#include "synthetic_harness.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define SC_SYNTH_PRESSURE_MAX 64

typedef struct {
    int reqs;
    int ok;
    int fail;
    double total_ms;
    double max_ms;
} sc_pressure_res_t;

static void pressure_worker(sc_allocator_t *alloc, uint16_t port, int dur_s, int wfd) {
    char hurl[128], curl[128];
    snprintf(hurl, sizeof(hurl), "http://127.0.0.1:%u/health", port);
    snprintf(curl, sizeof(curl), "http://127.0.0.1:%u/v1/chat/completions", port);
    sc_pressure_res_t r = {0};
    double deadline = sc_synth_now_ms() + (double)dur_s * 1000.0;
    unsigned seed = (unsigned)((unsigned)getpid() ^ (unsigned)(long)sc_synth_now_ms());
    while (sc_synth_now_ms() < deadline) {
        size_t ti = (size_t)(rand_r(&seed) % 4);
        sc_http_response_t resp = {0};
        sc_error_t e;
        double t0 = sc_synth_now_ms();
        if (ti == 0) {
            e = sc_http_get(alloc, hurl, NULL, &resp);
        } else if (ti == 1) {
            char u[128];
            snprintf(u, sizeof(u), "http://127.0.0.1:%u/v1/models", port);
            e = sc_http_get(alloc, u, NULL, &resp);
        } else if (ti == 2) {
            char u[128];
            snprintf(u, sizeof(u), "http://127.0.0.1:%u/api/status", port);
            e = sc_http_get(alloc, u, NULL, &resp);
        } else {
            static const char cb[] = "{\"model\":\"gemini-2.5-flash\","
                                     "\"messages\":[{\"role\":\"user\",\"content\":\"OK\"}],"
                                     "\"max_tokens\":5}";
            e = sc_http_post_json(alloc, curl, NULL, cb, sizeof(cb) - 1, &resp);
        }
        double lat = sc_synth_now_ms() - t0;
        r.reqs++;
        r.total_ms += lat;
        if (lat > r.max_ms)
            r.max_ms = lat;
        if (e == SC_OK && resp.status_code >= 200 && resp.status_code < 500)
            r.ok++;
        else
            r.fail++;
        sc_http_response_free(alloc, &resp);
    }
    ssize_t w = write(wfd, &r, sizeof(r));
    (void)w;
    close(wfd);
    _exit(0);
}

sc_error_t sc_synth_run_pressure(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                                 sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics) {
    (void)gemini;
    SC_SYNTH_LOG("=== Pressure Tests ===");
    sc_synth_metrics_init(metrics);
    int cc = cfg->concurrency > 0 ? cfg->concurrency : 4;
    int dur = cfg->duration_secs > 0 ? cfg->duration_secs : 10;
    if (cc > SC_SYNTH_PRESSURE_MAX)
        cc = SC_SYNTH_PRESSURE_MAX;
    SC_SYNTH_LOG("launching %d workers for %ds...", cc, dur);
    int pipes[SC_SYNTH_PRESSURE_MAX][2];
    pid_t pids[SC_SYNTH_PRESSURE_MAX];
    for (int i = 0; i < cc; i++) {
        if (pipe(pipes[i]) != 0)
            return SC_ERR_IO;
        pid_t p = fork();
        if (p < 0) {
            close(pipes[i][0]);
            close(pipes[i][1]);
            return SC_ERR_IO;
        }
        if (p == 0) {
            close(pipes[i][0]);
            for (int j = 0; j < i; j++)
                close(pipes[j][0]);
            pressure_worker(alloc, cfg->gateway_port, dur, pipes[i][1]);
        }
        pids[i] = p;
        close(pipes[i][1]);
    }
    int tr = 0, ts = 0, tf = 0;
    double tl = 0, ml = 0;
    for (int i = 0; i < cc; i++) {
        sc_pressure_res_t r = {0};
        ssize_t rd = read(pipes[i][0], &r, sizeof(r));
        close(pipes[i][0]);
        if (rd == (ssize_t)sizeof(r)) {
            tr += r.reqs;
            ts += r.ok;
            tf += r.fail;
            tl += r.total_ms;
            if (r.max_ms > ml)
                ml = r.max_ms;
        }
        int s = 0;
        waitpid(pids[i], &s, 0);
    }
    double avg = tr > 0 ? tl / (double)tr : 0;
    double rps = (double)tr / (double)dur;
    metrics->total = tr;
    metrics->passed = ts;
    metrics->failed = tf;
    sc_synth_metrics_record(alloc, metrics, avg, SC_SYNTH_PASS);
    SC_SYNTH_LOG(
        "Pressure: %d workers, %ds, %d reqs, %.1f req/s, %d ok, %d fail, avg %.1fms, max %.1fms",
        cc, dur, tr, rps, ts, tf, avg, ml);
    return SC_OK;
}
