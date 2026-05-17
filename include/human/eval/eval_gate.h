#ifndef HU_EVAL_GATE_H
#define HU_EVAL_GATE_H

/* Phase 5 Task 5 — statistical LoRA promotion gate. */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_leaderboard_runner;
struct hu_reward_model;

typedef struct hu_eval_gate {
    double baseline_persona_fidelity_mean;
    double baseline_mt_bench_mean;
    double baseline_ifeval_mean;
    double baseline_p95_latency_ms;
    double persona_delta_min;
    double mt_bench_regression_max;
    double ifeval_regression_max;
    double latency_delta_max_ms;
    size_t bootstrap_samples;
    uint32_t bootstrap_seed;
    struct hu_leaderboard_runner *mt_bench;
    struct hu_leaderboard_runner *ifeval;
    struct hu_reward_model *reward_model;
} hu_eval_gate_t;

typedef struct hu_eval_gate_verdict {
    bool promote;
    bool persona_pass;
    bool mt_bench_pass;
    bool ifeval_pass;
    bool latency_pass;
    bool reward_pass;
    double persona_ci_lower;
    double persona_ci_upper;
    double mt_ci_lower;
    double mt_ci_upper;
    double ifeval_ci_lower;
    double ifeval_ci_upper;
    double reward_ci_lower;
    double reward_ci_upper;
    char reason[512];
} hu_eval_gate_verdict_t;

hu_error_t hu_eval_gate_decide_from_arrays_for_test(
    const hu_eval_gate_t *gate, const double *persona, const double *mt_bench,
    const double *ifeval, const double *reward, size_t n, double candidate_p95_ms,
    hu_eval_gate_verdict_t *out);

#ifdef HU_IS_TEST
void hu_eval_gate_set_decide_spy_for_test(int *counter);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_GATE_H */
