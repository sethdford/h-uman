#include "human/eval_dashboard.h"
#include <stdio.h>
#include <string.h>

#define SUITE_COL_WIDTH 17
#define NUM_COL_WIDTH 6
#define RATE_COL_WIDTH 7
#define TIME_COL_WIDTH 6

static void truncate_suite_name(char *buf, size_t buf_size, const char *name) {
    if (!name) {
        buf[0] = '\0';
        return;
    }
    size_t len = strlen(name);
    if (len >= buf_size) {
        memcpy(buf, name, buf_size - 4);
        buf[buf_size - 4] = '.';
        buf[buf_size - 3] = '.';
        buf[buf_size - 2] = '.';
        buf[buf_size - 1] = '\0';
    } else {
        memcpy(buf, name, len + 1);
    }
}

hu_error_t hu_eval_dashboard_render(hu_allocator_t *alloc, FILE *out,
                                     const hu_eval_run_t *runs, size_t runs_count) {
    (void)alloc;
    if (!out) return HU_ERR_INVALID_ARGUMENT;

    fprintf(out, "╔═══════════════════════════════════════════════════════╗\n");
    fprintf(out, "║                   EVAL DASHBOARD                      ║\n");
    fprintf(out, "╠═══════════════════╤════════╤════════╤════════╤════════╣\n");
    fprintf(out, "║ Suite             │ Passed │ Failed │  Rate  │  Time  ║\n");
    fprintf(out, "╠═══════════════════╪════════╪════════╪════════╪════════╣\n");

    if (!runs || runs_count == 0) {
        fprintf(out, "║ (no runs)                                          ║\n");
        fprintf(out, "╠═══════════════════╧════════╧════════╧════════╧════════╣\n");
        fprintf(out, "║ Total: 0/0 passed (0.0%%) | 0ms elapsed               ║\n");
        fprintf(out, "╚═══════════════════════════════════════════════════════╝\n");
        return HU_OK;
    }

    size_t total_passed = 0;
    size_t total_failed = 0;
    int64_t total_elapsed = 0;

    for (size_t i = 0; i < runs_count; i++) {
        const hu_eval_run_t *r = &runs[i];
        size_t passed = r->passed;
        size_t failed = r->failed;
        double rate = (passed + failed) > 0 ? (100.0 * (double)passed / (double)(passed + failed)) : 0.0;
        int64_t elapsed = r->total_elapsed_ms;

        total_passed += passed;
        total_failed += failed;
        total_elapsed += elapsed;

        char suite_buf[SUITE_COL_WIDTH + 1];
        truncate_suite_name(suite_buf, sizeof(suite_buf), r->suite_name ? r->suite_name : "(unnamed)");

        fprintf(out, "║ %-15s │ %6zu │ %6zu │ %5.1f%% │ %5lldms ║\n",
                suite_buf, passed, failed, rate, (long long)elapsed);
    }

    size_t total = total_passed + total_failed;
    double total_rate = total > 0 ? (100.0 * (double)total_passed / (double)total) : 0.0;

    fprintf(out, "╠═══════════════════╧════════╧════════╧════════╧════════╣\n");
    fprintf(out, "║ Total: %zu/%zu passed (%.1f%%) | %lldms elapsed          ║\n",
            total_passed, total, total_rate, (long long)total_elapsed);
    fprintf(out, "╚═══════════════════════════════════════════════════════╝\n");

    return HU_OK;
}

static const char *eval_suite_key(const hu_eval_run_t *r) {
    if (!r || !r->suite_name)
        return "(unnamed)";
    return r->suite_name;
}

static double eval_run_pass_rate(const hu_eval_run_t *r) {
    if (!r)
        return 0.0;
    size_t total = r->passed + r->failed;
    if (total > 0)
        return 100.0 * (double)r->passed / (double)total;
    return 0.0;
}

static void eval_format_rate_cell(char *buf, size_t buf_size, const hu_eval_run_t *r, bool present) {
    if (!present || !r) {
        snprintf(buf, buf_size, "   —   ");
        return;
    }
    double rate = eval_run_pass_rate(r);
    snprintf(buf, buf_size, "%5.1f%%", rate);
}

static long eval_find_unused_current_match(const hu_eval_run_t *current, size_t current_count,
                                           const unsigned char *used, const hu_eval_run_t *baseline_row) {
    if (!current || current_count == 0 || !used || !baseline_row)
        return -1;
    const char *want = eval_suite_key(baseline_row);
    for (size_t j = 0; j < current_count; j++) {
        if (used[j])
            continue;
        if (strcmp(want, eval_suite_key(&current[j])) == 0)
            return (long)j;
    }
    return -1;
}

static void eval_aggregate_pass_totals(const hu_eval_run_t *runs, size_t runs_count, size_t *passed_out,
                                       size_t *failed_out) {
    *passed_out = 0;
    *failed_out = 0;
    if (!runs || runs_count == 0)
        return;
    for (size_t i = 0; i < runs_count; i++) {
        *passed_out += runs[i].passed;
        *failed_out += runs[i].failed;
    }
}

hu_error_t hu_eval_dashboard_render_trend(hu_allocator_t *alloc, FILE *out,
                                          const hu_eval_run_t *baseline, size_t baseline_count,
                                          const hu_eval_run_t *current, size_t current_count) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if ((baseline_count > 0 && !baseline) || (current_count > 0 && !current))
        return HU_ERR_INVALID_ARGUMENT;

    const bool need_used = baseline_count > 0 && current_count > 0;
    unsigned char *used = NULL;
    if (need_used) {
        if (!alloc || !alloc->alloc || !alloc->free)
            return HU_ERR_INVALID_ARGUMENT;
        used = (unsigned char *)alloc->alloc(alloc->ctx, current_count);
        if (!used)
            return HU_ERR_OUT_OF_MEMORY;
        memset(used, 0, current_count);
    }

    fprintf(out, "╔═══════════════════════════════════════════════════════════╗\n");
    fprintf(out, "║              EVAL DASHBOARD — TREND                     ║\n");
    fprintf(out, "╠═══════════════════╤══════════╤══════════╤════════════════╣\n");
    fprintf(out, "║ Suite             │ Baseline │ Current  │   Δ (pp)       ║\n");
    fprintf(out, "╠═══════════════════╪══════════╪══════════╪════════════════╣\n");

    if (baseline_count == 0 && current_count == 0) {
        fprintf(out, "║ (no runs)                                               ║\n");
        fprintf(out, "╠═══════════════════╧══════════╧══════════╧════════════════╣\n");
        fprintf(out, "║ Overall: baseline — | current — | Δ —                    ║\n");
        fprintf(out, "╚═══════════════════════════════════════════════════════════╝\n");
        if (used)
            alloc->free(alloc->ctx, used, current_count);
        return HU_OK;
    }

    for (size_t i = 0; i < baseline_count; i++) {
        const hu_eval_run_t *b = &baseline[i];
        long j = -1;
        if (current && current_count > 0 && used)
            j = eval_find_unused_current_match(current, current_count, used, b);
        if (j >= 0)
            used[(size_t)j] = 1;

        const hu_eval_run_t *c = (j >= 0) ? &current[(size_t)j] : NULL;

        char suite_buf[SUITE_COL_WIDTH + 1];
        truncate_suite_name(suite_buf, sizeof(suite_buf), eval_suite_key(b));

        char bcell[16];
        char ccell[16];
        char dcell[24];
        eval_format_rate_cell(bcell, sizeof(bcell), b, true);
        eval_format_rate_cell(ccell, sizeof(ccell), c, c != NULL);

        if (c) {
            double delta = eval_run_pass_rate(c) - eval_run_pass_rate(b);
            snprintf(dcell, sizeof(dcell), "%+7.1f", delta);
        } else {
            snprintf(dcell, sizeof(dcell), "   —   ");
        }

        fprintf(out, "║ %-15s │ %8s │ %8s │ %14s ║\n", suite_buf, bcell, ccell, dcell);
    }

    if (current && current_count > 0) {
        for (size_t j = 0; j < current_count; j++) {
            if (used && used[j])
                continue;

            const hu_eval_run_t *c = &current[j];
            char suite_buf[SUITE_COL_WIDTH + 1];
            truncate_suite_name(suite_buf, sizeof(suite_buf), eval_suite_key(c));

            char bcell[16];
            char ccell[16];
            eval_format_rate_cell(bcell, sizeof(bcell), NULL, false);
            eval_format_rate_cell(ccell, sizeof(ccell), c, true);

            fprintf(out, "║ %-15s │ %8s │ %8s │ %14s ║\n", suite_buf, bcell, ccell, "   —   ");
        }
    }

    size_t bp = 0, bf = 0, cp = 0, cf = 0;
    eval_aggregate_pass_totals(baseline, baseline_count, &bp, &bf);
    eval_aggregate_pass_totals(current, current_count, &cp, &cf);

    size_t bt = bp + bf;
    size_t ct = cp + cf;
    double brate = bt > 0 ? (100.0 * (double)bp / (double)bt) : 0.0;
    double crate = ct > 0 ? (100.0 * (double)cp / (double)ct) : 0.0;
    double odelta = crate - brate;

    fprintf(out, "╠═══════════════════╧══════════╧══════════╧════════════════╣\n");
    if (bt == 0 && ct == 0) {
        fprintf(out, "║ Overall: baseline — | current — | Δ —                    ║\n");
    } else if (bt == 0) {
        fprintf(out, "║ Overall: baseline — | current %5.1f%% | Δ —               ║\n", crate);
    } else if (ct == 0) {
        fprintf(out, "║ Overall: baseline %5.1f%% | current — | Δ —               ║\n", brate);
    } else {
        fprintf(out, "║ Overall: baseline %5.1f%% | current %5.1f%% | Δ %+6.1f pp        ║\n", brate, crate,
                odelta);
    }
    fprintf(out, "╚═══════════════════════════════════════════════════════════╝\n");

    if (used)
        alloc->free(alloc->ctx, used, current_count);
    return HU_OK;
}
