#include "human/eval/competitive_harness.h"

#include <stdio.h>
#include <string.h>

static hu_error_t write_text_file(const char *path, const char *body) {
    if (!path || !body)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fputs(body, f);
    fclose(f);
    return HU_OK;
}

hu_error_t hu_competitive_harness_run_with_test_judges(
    hu_allocator_t *alloc, const hu_competitive_harness_config_t *cfg,
    const hu_competitive_harness_judge_slot_t *judges, size_t n_judges,
    hu_competitive_harness_result_t *out) {
    if (!alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->n_columns = n_judges;

    size_t avail = 0;
    for (size_t i = 0; i < n_judges; i++) {
        if (judges && judges[i].available)
            avail++;
    }
    out->n_available = avail;

    size_t min_avail = cfg->min_available > 0 ? cfg->min_available : 1;

    double baseline_lo = 0.0;
    double baseline_hi = 0.0;
    double candidate_lo = 0.0;
    double candidate_hi = 0.0;
    bool have_baseline = false;
    bool have_candidate = false;
    const char *candidate_name = NULL;

    for (size_t i = 0; judges && i < n_judges; i++) {
        const hu_competitive_harness_judge_slot_t *j = &judges[i];
        if (!j->available || !j->has_persona_metrics)
            continue;
        if (j->is_baseline) {
            baseline_lo = j->ci_lower;
            baseline_hi = j->ci_upper;
            have_baseline = true;
        } else {
            candidate_lo = j->ci_lower;
            candidate_hi = j->ci_upper;
            have_candidate = true;
            candidate_name = j->column_name;
        }
    }

    bool win_met = have_baseline && have_candidate && candidate_lo > baseline_hi;
    char rationale[512];
    if (win_met && candidate_name) {
        snprintf(rationale, sizeof(rationale),
                 "persona_fidelity: %s CI [%.3f, %.3f] vs baseline [%.3f, %.3f]; "
                 "candidate lower > baseline upper",
                 candidate_name, candidate_lo, candidate_hi, baseline_lo, baseline_hi);
    } else if (have_baseline && have_candidate && candidate_name) {
        snprintf(rationale, sizeof(rationale),
                 "win condition not met: %s CI [%.3f, %.3f] vs baseline [%.3f, %.3f]",
                 candidate_name, candidate_lo, candidate_hi, baseline_lo, baseline_hi);
    } else {
        snprintf(rationale, sizeof(rationale),
                 "win condition not met (need candidate ci_lower > baseline ci_upper)");
    }

    snprintf(out->summary, sizeof(out->summary),
             "Run summary: %zu of %zu competitors available; win_condition_met=%s", avail,
             n_judges, win_met ? "true" : "false");

    char md[16384];
    size_t off = 0;
    off += (size_t)snprintf(md + off, sizeof(md) - off, "# Competitive scorecard\n\n%s\n\n%s\n\n",
                            out->summary, rationale);
    off += (size_t)snprintf(md + off, sizeof(md) - off,
                            "| column | persona_fidelity | CI | status |\n");
    off += (size_t)snprintf(md + off, sizeof(md) - off, "|--------|------------------|-----|--------|\n");

    for (size_t i = 0; judges && i < n_judges; i++) {
        const hu_competitive_harness_judge_slot_t *j = &judges[i];
        if (j->available && j->has_persona_metrics) {
            off += (size_t)snprintf(md + off, sizeof(md) - off,
                                    "| %s | %.3f | [%.3f, %.3f] | ok |\n",
                                    j->column_name ? j->column_name : "?",
                                    j->persona_fidelity, j->ci_lower, j->ci_upper);
        } else if (j->available) {
            off += (size_t)snprintf(md + off, sizeof(md) - off, "| %s | (v2 scorer) | — | ok |\n",
                                    j->column_name ? j->column_name : "?");
        } else {
            const char *reason =
                j->unavailable_reason ? j->unavailable_reason : "unavailable";
            off += (size_t)snprintf(md + off, sizeof(md) - off,
                                    "| %s | — | — | %s |\n",
                                    j->column_name ? j->column_name : "?", reason);
        }
    }

    if (cfg->out_markdown) {
        hu_error_t e = write_text_file(cfg->out_markdown, md);
        if (e != HU_OK)
            return e;
    }

    if (cfg->out_json) {
        char json[16384];
        size_t joff = 0;
        joff += (size_t)snprintf(json + joff, sizeof(json) - joff,
                                "{\"summary\":\"%s\",\"available\":%zu,\"n_columns\":%zu,"
                                "\"win_condition_met\":%s,\"win_condition_rationale\":\"%s\","
                                "\"columns\":[",
                                out->summary, avail, n_judges, win_met ? "true" : "false",
                                rationale);
        for (size_t i = 0; judges && i < n_judges; i++) {
            const hu_competitive_harness_judge_slot_t *j = &judges[i];
            joff += (size_t)snprintf(json + joff, sizeof(json) - joff, "%s{\"name\":\"%s\","
                                    "\"available\":%s",
                                    i ? "," : "", j->column_name ? j->column_name : "?",
                                    j->available ? "true" : "false");
            if (!j->available) {
                const char *reason =
                    j->unavailable_reason ? j->unavailable_reason : "unavailable";
                joff += (size_t)snprintf(json + joff, sizeof(json) - joff,
                                        ",\"unavailable_reason\":\"%s\"}", reason);
                continue;
            }
            if (j->has_persona_metrics) {
                joff += (size_t)snprintf(
                    json + joff, sizeof(json) - joff,
                    ",\"persona_fidelity\":%.6f,\"ci_lower\":%.6f,\"ci_upper\":%.6f,"
                    "\"n_samples\":%zu,\"p95_ms\":%.3f,\"is_baseline\":%s",
                    j->persona_fidelity, j->ci_lower, j->ci_upper, j->n_samples, j->p95_ms,
                    j->is_baseline ? "true" : "false");
                if (!j->is_baseline && have_baseline) {
                    joff += (size_t)snprintf(json + joff, sizeof(json) - joff,
                                            ",\"delta_vs_baseline\":%.6f,"
                                            "\"delta_ci_lower\":%.6f,\"delta_ci_upper\":%.6f",
                                            j->delta_vs_baseline, j->delta_ci_lower,
                                            j->delta_ci_upper);
                }
                joff += (size_t)snprintf(json + joff, sizeof(json) - joff, "}");
            } else {
                joff += (size_t)snprintf(json + joff, sizeof(json) - joff, "}");
            }
        }
        joff += (size_t)snprintf(json + joff, sizeof(json) - joff, "]}\n");
        hu_error_t e = write_text_file(cfg->out_json, json);
        if (e != HU_OK)
            return e;
    }

    if (avail < min_avail)
        return HU_ERR_NOT_SUPPORTED;
    (void)alloc;
    (void)cfg->prompt_fixture;
    return HU_OK;
}
