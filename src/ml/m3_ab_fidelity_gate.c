/* Spec 2026-05-19 M3 closure / AC-M3-5 — fidelity-based A/B gate.
 * See include/human/ml/m3_ab_fidelity_gate.h for the contract.
 *
 * Reads JSONL files line-by-line; for each row the "response" field
 * is fed to hu_communication_style_fidelity_score against the target
 * fingerprint. Per-row scores are aggregated into a
 * hu_communication_style_set_summary_t (mean, min, max, scored,
 * skipped). The verdict compares the two means via
 * hu_m3_ab_fidelity_pass.
 *
 * The implementation is read-only (no allocations beyond what the
 * JSON parser does internally; everything is freed before return) and
 * deterministic (no clock / RNG / network). Per
 * ~/.claude/rules/cross-language-via-http.md, this C code is the
 * scoring half; the live-fire script provides the inference half via
 * HTTP. */

#include "human/ml/m3_ab_fidelity_gate.h"

#include "human/core/json.h"
#include "human/core/log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool hu_m3_ab_fidelity_pass(float baseline_mean, float candidate_mean, float threshold) {
    if (threshold < 0.f)
        return false;
    /* Reject NaN / inf — these would produce a meaningless verdict. */
    if (!isfinite(baseline_mean) || !isfinite(candidate_mean) || !isfinite(threshold))
        return false;
    return (candidate_mean - baseline_mean) >= threshold;
}

/* Append a parsed response to the running summary. Mirrors the
 * accumulator shape used by hu_communication_style_compare_response_sets,
 * but operates on one response at a time so we can stream a JSONL
 * file without materializing the full set in memory. */
static void accumulate_score(const hu_communication_style_t *target, const char *response,
                             size_t response_len, hu_communication_style_set_summary_t *acc,
                             float *running_sum) {
    if (!response || response_len == 0) {
        acc->skipped++;
        return;
    }
    float s = hu_communication_style_fidelity_score(target, response, response_len);
    if (s < 0.f) {
        acc->skipped++;
        return;
    }
    *running_sum += s;
    if (acc->scored == 0) {
        acc->min_score = s;
        acc->max_score = s;
    } else {
        if (s < acc->min_score)
            acc->min_score = s;
        if (s > acc->max_score)
            acc->max_score = s;
    }
    acc->scored++;
}

hu_error_t hu_m3_ab_score_responses_jsonl(hu_allocator_t *alloc,
                                          const hu_communication_style_t *target,
                                          const char *responses_jsonl_path,
                                          hu_communication_style_set_summary_t *out_summary) {
    if (!alloc || !target || !responses_jsonl_path || !out_summary)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0U)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out_summary, 0, sizeof(*out_summary));
    out_summary->min_score = 1.f;
    out_summary->max_score = 0.f;

    FILE *fp = fopen(responses_jsonl_path, "rb");
    if (!fp)
        return HU_ERR_IO;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    rewind(fp);

    /* Empty file → zero scored, zero skipped. Not an error. */
    if (size == 0) {
        fclose(fp);
        return HU_OK;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t read_n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read_n] = '\0';

    float running_sum = 0.f;
    size_t i = 0;
    while (i < read_n) {
        size_t line_start = i;
        while (i < read_n && buf[i] != '\n')
            i++;
        size_t line_len = i - line_start;
        if (i < read_n)
            i++; /* consume newline */

        /* Trim leading whitespace. */
        size_t s = line_start;
        size_t e = line_start + line_len;
        while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r'))
            s++;
        while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t' || buf[e - 1] == '\r'))
            e--;
        if (s == e)
            continue; /* blank line */
        if (buf[s] == '#')
            continue; /* comment */

        hu_json_value_t *parsed = NULL;
        if (hu_json_parse(alloc, buf + s, e - s, &parsed) != HU_OK || !parsed) {
            out_summary->skipped++;
            continue;
        }
        if (parsed->type != HU_JSON_OBJECT) {
            hu_json_free(alloc, parsed);
            out_summary->skipped++;
            continue;
        }
        const char *response = hu_json_get_string(parsed, "response");
        if (!response) {
            hu_json_free(alloc, parsed);
            out_summary->skipped++;
            continue;
        }
        accumulate_score(target, response, strlen(response), out_summary, &running_sum);
        hu_json_free(alloc, parsed);
    }

    alloc->free(alloc->ctx, buf, (size_t)size + 1);

    if (out_summary->scored == 0) {
        out_summary->mean = 0.f;
        out_summary->min_score = 0.f;
        out_summary->max_score = 0.f;
    } else {
        out_summary->mean = running_sum / (float)out_summary->scored;
    }
    return HU_OK;
}

hu_error_t hu_m3_ab_run_fidelity_gate(hu_allocator_t *alloc, const hu_communication_style_t *target,
                                      const char *baseline_responses_jsonl,
                                      const char *candidate_responses_jsonl, float threshold,
                                      hu_m3_ab_fidelity_report_t *out_report) {
    if (!alloc || !target || !baseline_responses_jsonl || !candidate_responses_jsonl || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    if (target->sample_count == 0U)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out_report, 0, sizeof(*out_report));
    if (threshold < 0.f)
        threshold = HU_M3_AB_FIDELITY_THRESHOLD_DEFAULT;
    out_report->threshold = threshold;

    hu_error_t err = hu_m3_ab_score_responses_jsonl(alloc, target, baseline_responses_jsonl,
                                                    &out_report->baseline);
    if (err != HU_OK)
        return err;
    err = hu_m3_ab_score_responses_jsonl(alloc, target, candidate_responses_jsonl,
                                         &out_report->candidate);
    if (err != HU_OK)
        return err;

    out_report->delta = out_report->candidate.mean - out_report->baseline.mean;

    if (out_report->baseline.scored == 0) {
        out_report->pass = false;
        out_report->reason = "baseline JSONL produced zero scored responses";
        return HU_OK;
    }
    if (out_report->candidate.scored == 0) {
        out_report->pass = false;
        out_report->reason = "candidate JSONL produced zero scored responses";
        return HU_OK;
    }

    out_report->pass =
        hu_m3_ab_fidelity_pass(out_report->baseline.mean, out_report->candidate.mean, threshold);
    if (!out_report->pass) {
        out_report->reason = "candidate fidelity delta below threshold";
    }
    return HU_OK;
}
