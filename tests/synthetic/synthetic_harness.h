#ifndef SC_SYNTHETIC_HARNESS_H
#define SC_SYNTHETIC_HARNESS_H
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
typedef struct sc_synth_config {
    const char *binary_path;
    const char *gemini_api_key;
    const char *gemini_model;
    uint16_t gateway_port;
    int concurrency;
    int duration_secs;
    int tests_per_category;
    const char *regression_dir;
    bool cli_only;
    bool gateway_only;
    bool ws_only;
    bool agent_only;
    bool pressure_only;
    bool replay_mode;
    const char *replay_dir;
    bool verbose;
} sc_synth_config_t;
typedef enum sc_synth_verdict {
    SC_SYNTH_PASS = 0,
    SC_SYNTH_FAIL,
    SC_SYNTH_ERROR,
    SC_SYNTH_SKIP,
} sc_synth_verdict_t;
static inline const char *sc_synth_verdict_str(sc_synth_verdict_t v) {
    switch (v) {
    case SC_SYNTH_PASS:
        return "PASS";
    case SC_SYNTH_FAIL:
        return "FAIL";
    case SC_SYNTH_ERROR:
        return "ERROR";
    case SC_SYNTH_SKIP:
        return "SKIP";
    }
    return "UNKNOWN";
}
typedef struct sc_synth_test_case {
    char *name;
    char *category;
    char *input_json;
    char *expected_json;
    sc_synth_verdict_t verdict;
    char *actual_output;
    char *verdict_reason;
    double latency_ms;
} sc_synth_test_case_t;
void sc_synth_test_case_free(sc_allocator_t *alloc, sc_synth_test_case_t *tc);
typedef struct sc_synth_metrics {
    double *latencies;
    size_t latency_count;
    size_t latency_cap;
    int total;
    int passed;
    int failed;
    int errors;
    int skipped;
} sc_synth_metrics_t;
void sc_synth_metrics_init(sc_synth_metrics_t *m);
void sc_synth_metrics_record(sc_allocator_t *alloc, sc_synth_metrics_t *m, double latency_ms,
                             sc_synth_verdict_t v);
void sc_synth_metrics_free(sc_allocator_t *alloc, sc_synth_metrics_t *m);
double sc_synth_metrics_avg(const sc_synth_metrics_t *m);
double sc_synth_metrics_percentile(const sc_synth_metrics_t *m, double pct);
double sc_synth_metrics_max(const sc_synth_metrics_t *m);
static inline double sc_synth_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}
static inline char *sc_synth_strdup(sc_allocator_t *alloc, const char *s, size_t len) {
    if (!s)
        return NULL;
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}
static inline void sc_synth_strfree(sc_allocator_t *alloc, char *s, size_t len) {
    if (s)
        alloc->free(alloc->ctx, s, len + 1);
}
typedef struct sc_synth_gemini_ctx sc_synth_gemini_ctx_t;
sc_error_t sc_synth_gemini_init(sc_allocator_t *alloc, const char *api_key, const char *model,
                                sc_synth_gemini_ctx_t **out);
sc_error_t sc_synth_gemini_generate(sc_allocator_t *alloc, sc_synth_gemini_ctx_t *ctx,
                                    const char *prompt, size_t prompt_len, char **response_out,
                                    size_t *response_len_out);
sc_error_t sc_synth_gemini_evaluate(sc_allocator_t *alloc, sc_synth_gemini_ctx_t *ctx,
                                    const char *test_desc, const char *actual_output,
                                    sc_synth_verdict_t *verdict_out, char **reason_out,
                                    size_t *reason_len_out);
void sc_synth_gemini_deinit(sc_allocator_t *alloc, sc_synth_gemini_ctx_t *ctx);
sc_error_t sc_synth_run_cli(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                            sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics);
sc_error_t sc_synth_run_gateway(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                                sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics);
sc_error_t sc_synth_run_ws(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                           sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics);
sc_error_t sc_synth_run_agent(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                              sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics);
sc_error_t sc_synth_run_pressure(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                                 sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics);
sc_error_t sc_synth_regression_save(sc_allocator_t *alloc, const char *dir,
                                    const sc_synth_test_case_t *tc);
sc_error_t sc_synth_regression_load(sc_allocator_t *alloc, const char *dir,
                                    sc_synth_test_case_t **tests_out, size_t *count_out);
void sc_synth_regression_free_tests(sc_allocator_t *alloc, sc_synth_test_case_t *tests,
                                    size_t count);
void sc_synth_report_category(const char *name, const sc_synth_metrics_t *m);
void sc_synth_report_final(void);
#include <sys/types.h>
pid_t sc_synth_gateway_start(const char *binary, uint16_t port, char *tmpdir_out,
                             size_t tmpdir_len);
bool sc_synth_gateway_wait(sc_allocator_t *alloc, uint16_t port, int timeout_secs);
void sc_synth_gateway_stop(pid_t pid);
void sc_synth_gateway_cleanup(const char *tmpdir);
#define SC_SYNTH_LOG(fmt, ...) fprintf(stderr, "[synth] " fmt "\n", ##__VA_ARGS__)
#define SC_SYNTH_VERBOSE(cfg, fmt, ...)                        \
    do {                                                       \
        if ((cfg)->verbose)                                    \
            fprintf(stderr, "  [v] " fmt "\n", ##__VA_ARGS__); \
    } while (0)
#endif
