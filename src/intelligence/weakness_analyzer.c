/*
 * Weakness Analyzer (aggregated) — group eval failures by type, compute severity.
 * For closed-loop self-improvement: eval → weakness → fix → verify.
 */

#include "human/intelligence/weakness.h"
#include "human/core/string.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WEAKNESS_TYPE_COUNT 5

static hu_weakness_type_t classify_by_category(const char *cat) {
    if (!cat || cat[0] == '\0') return HU_WEAKNESS_UNKNOWN;
    size_t len = strlen(cat);
    if (hu_str_contains_ci(cat, len, "math", 4) ||
        hu_str_contains_ci(cat, len, "reason", 6) ||
        hu_str_contains_ci(cat, len, "logic", 5) ||
        hu_str_contains_ci(cat, len, "arithmetic", 10))
        return HU_WEAKNESS_REASONING;
    if (hu_str_contains_ci(cat, len, "knowledge", 9) ||
        hu_str_contains_ci(cat, len, "fact", 4) ||
        hu_str_contains_ci(cat, len, "trivia", 6) ||
        hu_str_contains_ci(cat, len, "geo", 3) ||
        hu_str_contains_ci(cat, len, "history", 7) ||
        hu_str_contains_ci(cat, len, "science", 7))
        return HU_WEAKNESS_KNOWLEDGE;
    if (hu_str_contains_ci(cat, len, "tool", 4) ||
        hu_str_contains_ci(cat, len, "function", 8) ||
        hu_str_contains_ci(cat, len, "api", 3))
        return HU_WEAKNESS_TOOL_USE;
    if (hu_str_contains_ci(cat, len, "code", 4) ||
        hu_str_contains_ci(cat, len, "coding", 6))
        return HU_WEAKNESS_TOOL_USE;  /* map to TOOL_USE for now */
    if (hu_str_contains_ci(cat, len, "format", 6) ||
        hu_str_contains_ci(cat, len, "output", 6) ||
        hu_str_contains_ci(cat, len, "style", 5) ||
        hu_str_contains_ci(cat, len, "instruction", 11) ||
        hu_str_contains_ci(cat, len, "follow", 6))
        return HU_WEAKNESS_FORMAT;
    return HU_WEAKNESS_UNKNOWN;
}

static hu_weakness_type_t classify_by_output(const char *actual, size_t actual_len,
                                             const char *expected, size_t expected_len) {
    if (!actual || actual_len == 0)
        return HU_WEAKNESS_TOOL_USE;

    int expected_is_short = (expected_len > 0 && expected_len <= 32);
    int actual_is_long = (actual_len > expected_len * 3 && expected_is_short);
    if (actual_is_long)
        return HU_WEAKNESS_FORMAT;

    if (expected_len > 0 && actual_len > 0) {
        int exp_has_digit = 0, act_has_digit = 0;
        for (size_t i = 0; i < expected_len; i++)
            if (isdigit((unsigned char)expected[i])) { exp_has_digit = 1; break; }
        for (size_t i = 0; i < actual_len; i++)
            if (isdigit((unsigned char)actual[i])) { act_has_digit = 1; break; }
        if (exp_has_digit && act_has_digit)
            return HU_WEAKNESS_REASONING;
    }

    return HU_WEAKNESS_UNKNOWN;
}

hu_error_t hu_weakness_analyze_summary(hu_allocator_t *alloc,
                                       const hu_eval_run_t *run,
                                       const hu_eval_suite_t *suite,
                                       hu_weakness_summary_t *summaries,
                                       size_t max_summaries,
                                       size_t *out_count) {
    (void)alloc;
    if (!run || !summaries || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    if (run->results_count == 0)
        return HU_OK;

    if (max_summaries == 0)
        return HU_OK;

    /* Accumulate: by_type[type] = { failed, total } */
    int failed_by_type[WEAKNESS_TYPE_COUNT];
    int total_by_type[WEAKNESS_TYPE_COUNT];
    char desc_by_type[WEAKNESS_TYPE_COUNT][512];
    memset(failed_by_type, 0, sizeof(failed_by_type));
    memset(total_by_type, 0, sizeof(total_by_type));
    memset(desc_by_type, 0, sizeof(desc_by_type));

    for (size_t i = 0; i < run->results_count; i++) {
        const hu_eval_result_t *r = &run->results[i];
        const hu_eval_task_t *task = NULL;
        if (suite && suite->tasks && r->task_id) {
            for (size_t t = 0; t < suite->tasks_count; t++) {
                if (suite->tasks[t].id && strcmp(suite->tasks[t].id, r->task_id) == 0) {
                    task = &suite->tasks[t];
                    break;
                }
            }
        }

        hu_weakness_type_t ty = HU_WEAKNESS_UNKNOWN;
        if (task && task->category)
            ty = classify_by_category(task->category);
        if (ty == HU_WEAKNESS_UNKNOWN)
            ty = classify_by_output(r->actual_output ? r->actual_output : "",
                                   r->actual_output_len,
                                   task && task->expected ? task->expected : "",
                                   task ? task->expected_len : 0);

        if (ty >= WEAKNESS_TYPE_COUNT)
            ty = HU_WEAKNESS_UNKNOWN;

        total_by_type[ty]++;
        if (!r->passed)
            failed_by_type[ty]++;

        if (desc_by_type[ty][0] == '\0' && !r->passed) {
            int n = snprintf(desc_by_type[ty], sizeof(desc_by_type[ty]),
                            "Failed task '%s': expected '%.64s', got '%.64s'",
                            r->task_id ? r->task_id : "(unknown)",
                            task && task->expected ? task->expected : "(unknown)",
                            r->actual_output ? r->actual_output : "(empty)");
            (void)n;
        }
    }

    size_t idx = 0;
    for (int ty = 0; ty < WEAKNESS_TYPE_COUNT && idx < max_summaries; ty++) {
        if (failed_by_type[ty] == 0)
            continue;

        hu_weakness_summary_t *s = &summaries[idx];
        s->type = (hu_weakness_type_t)ty;
        s->failed_count = failed_by_type[ty];
        s->total_count = total_by_type[ty];
        s->severity = (s->total_count > 0)
            ? (double)s->failed_count / (double)s->total_count
            : 0.0;
        if (s->severity > 1.0) s->severity = 1.0;
        if (s->severity < 0.0) s->severity = 0.0;

        size_t dlen = strlen(desc_by_type[ty]);
        if (dlen >= sizeof(s->description)) dlen = sizeof(s->description) - 1;
        memcpy(s->description, desc_by_type[ty], dlen);
        s->description[dlen] = '\0';
        s->description_len = dlen;

        idx++;
    }
    *out_count = idx;
    return HU_OK;
}
