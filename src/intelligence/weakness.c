/*
 * Weakness Analyzer — categorize eval failures into structured weaknesses.
 * Consumes hu_eval_run_t results and produces hu_weakness_report_t.
 */

#include "human/intelligence/weakness.h"
#include "human/core/string.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

const char *hu_weakness_type_str(hu_weakness_type_t type) {
    switch (type) {
    case HU_WEAKNESS_REASONING: return "reasoning";
    case HU_WEAKNESS_KNOWLEDGE: return "knowledge";
    case HU_WEAKNESS_TOOL_USE:  return "tool_use";
    case HU_WEAKNESS_FORMAT:    return "format";
    case HU_WEAKNESS_UNKNOWN:   return "unknown";
    }
    return "unknown";
}

const char *hu_weakness_type_name(hu_weakness_type_t type) {
    return hu_weakness_type_str(type);
}

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
        hu_str_contains_ci(cat, len, "api", 3) ||
        hu_str_contains_ci(cat, len, "code", 4))
        return HU_WEAKNESS_TOOL_USE;
    if (hu_str_contains_ci(cat, len, "format", 6) ||
        hu_str_contains_ci(cat, len, "output", 6) ||
        hu_str_contains_ci(cat, len, "style", 5))
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

static void build_description(hu_weakness_t *w, const hu_eval_result_t *r,
                              const hu_eval_task_t *task) {
    int n = snprintf(w->description, sizeof(w->description),
                     "Failed task '%s': expected '%.64s', got '%.64s'",
                     task ? task->id : r->task_id,
                     task ? task->expected : "(unknown)",
                     r->actual_output ? r->actual_output : "(empty)");
    w->description_len = (n > 0 && (size_t)n < sizeof(w->description))
                         ? (size_t)n : sizeof(w->description) - 1;
}

static void build_fix(hu_weakness_t *w) {
    const char *fix = NULL;
    switch (w->type) {
    case HU_WEAKNESS_REASONING:
        fix = "Show step-by-step reasoning before the final answer.";
        break;
    case HU_WEAKNESS_KNOWLEDGE:
        fix = "If unsure about factual claims, state uncertainty explicitly.";
        break;
    case HU_WEAKNESS_TOOL_USE:
        fix = "Always use available tools before attempting to answer from memory.";
        break;
    case HU_WEAKNESS_FORMAT:
        fix = "Match the expected output format exactly.";
        break;
    case HU_WEAKNESS_UNKNOWN:
        fix = "Review the task requirements and expected output carefully.";
        break;
    }
    size_t len = strlen(fix);
    if (len >= sizeof(w->suggested_fix)) len = sizeof(w->suggested_fix) - 1;
    memcpy(w->suggested_fix, fix, len);
    w->suggested_fix[len] = '\0';
    w->suggested_fix_len = len;
}

hu_error_t hu_weakness_analyze(hu_allocator_t *alloc, const hu_eval_run_t *run,
                               const hu_eval_suite_t *suite,
                               hu_weakness_report_t *out) {
    if (!alloc || !run || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    size_t fail_count = 0;
    for (size_t i = 0; i < run->results_count; i++) {
        if (!run->results[i].passed)
            fail_count++;
    }
    if (fail_count == 0)
        return HU_OK;

    out->items = (hu_weakness_t *)alloc->alloc(alloc->ctx,
                                               fail_count * sizeof(hu_weakness_t));
    if (!out->items)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out->items, 0, fail_count * sizeof(hu_weakness_t));

    size_t idx = 0;
    for (size_t i = 0; i < run->results_count; i++) {
        hu_eval_result_t *r = &run->results[i];
        if (r->passed)
            continue;

        hu_weakness_t *w = &out->items[idx];

        const hu_eval_task_t *task = NULL;
        if (suite && suite->tasks && r->task_id) {
            for (size_t t = 0; t < suite->tasks_count; t++) {
                if (suite->tasks[t].id &&
                    strcmp(suite->tasks[t].id, r->task_id) == 0) {
                    task = &suite->tasks[t];
                    break;
                }
            }
        }

        hu_weakness_type_t cat_type = HU_WEAKNESS_UNKNOWN;
        if (task && task->category)
            cat_type = classify_by_category(task->category);

        if (cat_type == HU_WEAKNESS_UNKNOWN) {
            cat_type = classify_by_output(
                r->actual_output, r->actual_output_len,
                task ? task->expected : NULL,
                task ? task->expected_len : 0);
        }

        w->type = cat_type;

        if (r->task_id) {
            size_t tl = strlen(r->task_id);
            if (tl >= sizeof(w->task_id)) tl = sizeof(w->task_id) - 1;
            memcpy(w->task_id, r->task_id, tl);
            w->task_id[tl] = '\0';
        }

        if (task && task->category) {
            size_t cl = strlen(task->category);
            if (cl >= sizeof(w->category)) cl = sizeof(w->category) - 1;
            memcpy(w->category, task->category, cl);
            w->category[cl] = '\0';
        }

        build_description(w, r, task);
        build_fix(w);

        out->by_type[w->type]++;
        idx++;
    }
    out->count = idx;
    return HU_OK;
}

void hu_weakness_report_free(hu_allocator_t *alloc, hu_weakness_report_t *report) {
    if (!alloc || !report)
        return;
    if (report->items) {
        alloc->free(alloc->ctx, report->items, report->count * sizeof(hu_weakness_t));
        report->items = NULL;
    }
    report->count = 0;
    memset(report->by_type, 0, sizeof(report->by_type));
}
