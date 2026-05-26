#include "human/eval/eval_gate.h"

#include "human/eval/bootstrap_ci.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_IS_TEST
static int *g_eval_gate_decide_spy = NULL;

void hu_eval_gate_set_decide_spy_for_test(int *counter) {
    g_eval_gate_decide_spy = counter;
}
#endif

static bool gate_ci_passes_lower(const double *xs, size_t n, size_t B, uint32_t seed,
                                 double baseline, double delta_min, double *out_lo,
                                 double *out_hi) {
    double lo = 0, hi = 0, mean = 0;
    if (hu_bootstrap_ci_for_test(xs, n, 0.95, B, seed, &lo, &hi, &mean) != HU_OK)
        return false;
    if (out_lo)
        *out_lo = lo;
    if (out_hi)
        *out_hi = hi;
    return lo > baseline + delta_min;
}

hu_error_t hu_eval_gate_decide_from_arrays_for_test(const hu_eval_gate_t *gate,
                                                    const double *persona, const double *mt_bench,
                                                    const double *ifeval, const double *reward,
                                                    size_t n, double candidate_p95_ms,
                                                    hu_eval_gate_verdict_t *out) {
#ifdef HU_IS_TEST
    if (g_eval_gate_decide_spy)
        (*g_eval_gate_decide_spy)++;
#endif
    if (!gate || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!persona)
        return HU_ERR_INVALID_ARGUMENT;
    if (gate->mt_bench && !mt_bench)
        return HU_ERR_INVALID_ARGUMENT;
    if (gate->ifeval && !ifeval)
        return HU_ERR_INVALID_ARGUMENT;
    if (gate->reward_model && !reward)
        return HU_ERR_INVALID_ARGUMENT;
    if (n < 1)
        return HU_ERR_INVALID_ARGUMENT;
    if (n < 10)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    size_t B = gate->bootstrap_samples > 0 ? gate->bootstrap_samples : 1000;
    uint32_t seed = gate->bootstrap_seed;

    out->persona_pass = gate_ci_passes_lower(
        persona, n, B, seed, gate->baseline_persona_fidelity_mean, gate->persona_delta_min,
        &out->persona_ci_lower, &out->persona_ci_upper);

    if (gate->mt_bench && mt_bench) {
        out->mt_bench_pass = gate_ci_passes_lower(
            mt_bench, n, B, seed, gate->baseline_mt_bench_mean, gate->mt_bench_regression_max,
            &out->mt_ci_lower, &out->mt_ci_upper);
    } else {
        out->mt_bench_pass = true;
        strncat(out->reason, "mt_bench: skipped (NULL runner); ", sizeof(out->reason) - 1);
    }

    if (gate->ifeval && ifeval) {
        out->ifeval_pass = gate_ci_passes_lower(ifeval, n, B, seed, gate->baseline_ifeval_mean,
                                                gate->ifeval_regression_max, &out->ifeval_ci_lower,
                                                &out->ifeval_ci_upper);
    } else {
        out->ifeval_pass = true;
        strncat(out->reason, "ifeval: skipped (NULL runner); ", sizeof(out->reason) - 1);
    }

    /* Latency gate: when BOTH baseline and delta_max are unset (zero), skip
     * the check — matches the "if unset, skip" pattern the other gates use
     * (mt_bench/ifeval/reward all default to pass when the runner is NULL).
     * Without this guard, an unset gate produces `candidate <= 0` which is
     * impossible to pass and silently blocks every promotion. Pinned by
     * tests/test_lora_training_runner_eval_gate.c::test_runner_promotes_
     * measured_gate_scores. */
    if (gate->baseline_p95_latency_ms <= 0.0 && gate->latency_delta_max_ms <= 0.0) {
        out->latency_pass = true;
        strncat(out->reason, "latency: skipped (no baseline/delta set); ", sizeof(out->reason) - 1);
    } else {
        out->latency_pass =
            candidate_p95_ms <= gate->baseline_p95_latency_ms + gate->latency_delta_max_ms;
    }

    if (gate->reward_model && reward) {
        out->reward_pass = gate_ci_passes_lower(reward, n, B, seed, 0.0, 0.0, &out->reward_ci_lower,
                                                &out->reward_ci_upper);
    } else {
        out->reward_pass = true;
        strncat(out->reason, "reward: skipped (NULL model); ", sizeof(out->reason) - 1);
    }

    out->promote = out->persona_pass && out->mt_bench_pass && out->ifeval_pass &&
                   out->latency_pass && out->reward_pass;

    if (!out->promote) {
        if (!out->persona_pass)
            strncat(out->reason, "persona fidelity CI below threshold; ", sizeof(out->reason) - 1);
        if (!out->mt_bench_pass)
            strncat(out->reason, "mt_bench regression; ", sizeof(out->reason) - 1);
        if (!out->ifeval_pass)
            strncat(out->reason, "ifeval regression; ", sizeof(out->reason) - 1);
        if (!out->latency_pass)
            strncat(out->reason, "latency p95 over budget; ", sizeof(out->reason) - 1);
        if (!out->reward_pass)
            strncat(out->reason, "reward CI below zero; ", sizeof(out->reason) - 1);
    }

    return HU_OK;
}
