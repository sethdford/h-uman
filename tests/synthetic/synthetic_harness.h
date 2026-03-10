#ifndef HU_SYNTHETIC_HARNESS_H
#define HU_SYNTHETIC_HARNESS_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
typedef struct hu_synth_config {
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
} hu_synth_config_t;
typedef enum hu_synth_verdict {
    HU_SYNTH_PASS = 0,
    HU_SYNTH_FAIL,
    HU_SYNTH_ERROR,
    HU_SYNTH_SKIP,
} hu_synth_verdict_t;
static inline const char *hu_synth_verdict_str(hu_synth_verdict_t v) {
    switch (v) {
    case HU_SYNTH_PASS:
        return "PASS";
    case HU_SYNTH_FAIL:
        return "FAIL";
    case HU_SYNTH_ERROR:
        return "ERROR";
    case HU_SYNTH_SKIP:
        return "SKIP";
    }
    return "UNKNOWN";
}
typedef struct hu_synth_test_case {
    char *name;
    char *category;
    char *input_json;
    char *expected_json;
    hu_synth_verdict_t verdict;
    char *actual_output;
    char *verdict_reason;
    double latency_ms;
} hu_synth_test_case_t;
void hu_synth_test_case_free(hu_allocator_t *alloc, hu_synth_test_case_t *tc);
typedef struct hu_synth_metrics {
    double *latencies;
    size_t latency_count;
    size_t latency_cap;
    int total;
    int passed;
    int failed;
    int errors;
    int skipped;
} hu_synth_metrics_t;
void hu_synth_metrics_init(hu_synth_metrics_t *m);
void hu_synth_metrics_record(hu_allocator_t *alloc, hu_synth_metrics_t *m, double latency_ms,
                             hu_synth_verdict_t v);
void hu_synth_metrics_free(hu_allocator_t *alloc, hu_synth_metrics_t *m);
double hu_synth_metrics_avg(const hu_synth_metrics_t *m);
double hu_synth_metrics_percentile(const hu_synth_metrics_t *m, double pct);
double hu_synth_metrics_max(const hu_synth_metrics_t *m);
static inline double hu_synth_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}
static inline char *hu_synth_strdup(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s)
        return NULL;
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}
static inline void hu_synth_strfree(hu_allocator_t *alloc, char *s, size_t len) {
    if (s)
        alloc->free(alloc->ctx, s, len + 1);
}
typedef struct hu_synth_gemini_ctx hu_synth_gemini_ctx_t;
hu_error_t hu_synth_gemini_init(hu_allocator_t *alloc, const char *api_key, const char *model,
                                hu_synth_gemini_ctx_t **out);
hu_error_t hu_synth_gemini_generate(hu_allocator_t *alloc, hu_synth_gemini_ctx_t *ctx,
                                    const char *prompt, size_t prompt_len, char **response_out,
                                    size_t *response_len_out);
hu_error_t hu_synth_gemini_evaluate(hu_allocator_t *alloc, hu_synth_gemini_ctx_t *ctx,
                                    const char *test_desc, const char *actual_output,
                                    hu_synth_verdict_t *verdict_out, char **reason_out,
                                    size_t *reason_len_out);
void hu_synth_gemini_deinit(hu_allocator_t *alloc, hu_synth_gemini_ctx_t *ctx);
hu_error_t hu_synth_run_cli(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                            hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_synth_run_gateway(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                                hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_synth_run_ws(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                           hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_synth_run_agent(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                              hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_synth_run_pressure(hu_allocator_t *alloc, const hu_synth_config_t *cfg,
                                 hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_synth_regression_save(hu_allocator_t *alloc, const char *dir,
                                    const hu_synth_test_case_t *tc);
hu_error_t hu_synth_regression_load(hu_allocator_t *alloc, const char *dir,
                                    hu_synth_test_case_t **tests_out, size_t *count_out);
void hu_synth_regression_free_tests(hu_allocator_t *alloc, hu_synth_test_case_t *tests,
                                    size_t count);
void hu_synth_report_category(const char *name, const hu_synth_metrics_t *m);
void hu_synth_report_final(void);
#include <sys/types.h>
pid_t hu_synth_gateway_start(const hu_synth_config_t *cfg, char *tmpdir_out, size_t tmpdir_len);
bool hu_synth_gateway_wait(hu_allocator_t *alloc, uint16_t port, int timeout_secs);
void hu_synth_gateway_stop(pid_t pid);
void hu_synth_gateway_cleanup(const char *tmpdir);
#define HU_SYNTH_LOG(fmt, ...) fprintf(stderr, "[synth] " fmt "\n", ##__VA_ARGS__)
#define HU_SYNTH_VERBOSE(cfg, fmt, ...)                        \
    do {                                                       \
        if ((cfg)->verbose)                                    \
            fprintf(stderr, "  [v] " fmt "\n", ##__VA_ARGS__); \
    } while (0)
#endif
