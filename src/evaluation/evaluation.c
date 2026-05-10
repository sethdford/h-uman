/* W16 — Evaluation dispatcher + report serialisation.
 *
 * Owns nothing beyond the wrapper struct: backends carry their own ctx and
 * are responsible for populating reports. This file keeps the dispatch path
 * boring (validate, forward, return) and the JSON shape stable so the
 * baseline file and CI artifacts stay diff-friendly.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── small string helpers ───────────────────────────────────────────────── */

static char *clone_cstr(hu_allocator_t *alloc, const char *s) {
    if (!alloc || !s)
        return NULL;
    return hu_strdup(alloc, s);
}

static void free_cstr(hu_allocator_t *alloc, char *s) {
    if (!alloc || !s)
        return;
    alloc->free(alloc->ctx, s, strlen(s) + 1);
}

/* ── report init / append helpers ───────────────────────────────────────── */

hu_error_t hu_evaluation_report_init(hu_allocator_t *alloc, const char *suite_name,
                                     hu_evaluation_run_report_t *out) {
    if (!alloc || !suite_name || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->suite_name = clone_cstr(alloc, suite_name);
    if (!out->suite_name)
        return HU_ERR_OUT_OF_MEMORY;
    out->metrics = alloc->alloc(alloc->ctx,
                                HU_EVALUATION_MAX_METRICS * sizeof(hu_evaluation_metric_t));
    if (!out->metrics) {
        free_cstr(alloc, out->suite_name);
        out->suite_name = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(out->metrics, 0, HU_EVALUATION_MAX_METRICS * sizeof(hu_evaluation_metric_t));
    out->metrics_count = 0;
    return HU_OK;
}

hu_error_t hu_evaluation_report_add_metric(hu_allocator_t *alloc, hu_evaluation_run_report_t *r,
                                           const char *name, double score, size_t sample_count) {
    if (!alloc || !r || !name || !r->metrics)
        return HU_ERR_INVALID_ARGUMENT;
    if (r->metrics_count >= HU_EVALUATION_MAX_METRICS)
        return HU_ERR_OUT_OF_MEMORY;
    if (score < 0.0 || isnan(score) || isinf(score))
        score = 0.0;
    if (score > 1.0)
        score = 1.0;

    hu_evaluation_metric_t *m = &r->metrics[r->metrics_count];
    m->name = clone_cstr(alloc, name);
    if (!m->name)
        return HU_ERR_OUT_OF_MEMORY;
    m->score = score;
    m->baseline = NAN;
    m->sample_count = sample_count;
    r->metrics_count++;
    return HU_OK;
}

hu_error_t hu_evaluation_report_set_error(hu_allocator_t *alloc, hu_evaluation_run_report_t *r,
                                          const char *summary) {
    if (!alloc || !r)
        return HU_ERR_INVALID_ARGUMENT;
    if (r->error_summary) {
        free_cstr(alloc, r->error_summary);
        r->error_summary = NULL;
    }
    if (!summary)
        return HU_OK;
    r->error_summary = clone_cstr(alloc, summary);
    return r->error_summary ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_evaluation_report_set_model(hu_allocator_t *alloc, hu_evaluation_run_report_t *r,
                                          const char *model_version) {
    if (!alloc || !r)
        return HU_ERR_INVALID_ARGUMENT;
    if (r->model_version) {
        free_cstr(alloc, r->model_version);
        r->model_version = NULL;
    }
    if (!model_version)
        return HU_OK;
    r->model_version = clone_cstr(alloc, model_version);
    return r->model_version ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

/* ── public dispatcher ──────────────────────────────────────────────────── */

hu_error_t hu_evaluation_run_suite(hu_evaluation_t *e, hu_evaluation_run_report_t *out) {
    if (!e || !e->vtable || !e->vtable->run || !e->alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    return e->vtable->run(e->ctx, e->alloc, out);
}

const char *hu_evaluation_get_name(const hu_evaluation_t *e) {
    if (!e || !e->vtable || !e->vtable->name)
        return NULL;
    return e->vtable->name(e->ctx);
}

bool hu_evaluation_is_available(const hu_evaluation_t *e) {
    if (!e || !e->vtable || !e->vtable->available)
        return false;
    return e->vtable->available(e->ctx);
}

void hu_evaluation_close(hu_evaluation_t *e) {
    if (!e || !e->vtable)
        return;
    if (e->vtable->deinit)
        e->vtable->deinit(e->ctx, e->alloc);
    memset(e, 0, sizeof(*e));
}

/* ── report free ────────────────────────────────────────────────────────── */

void hu_evaluation_report_free(hu_allocator_t *alloc, hu_evaluation_run_report_t *r) {
    if (!alloc || !r)
        return;
    free_cstr(alloc, r->suite_name);
    free_cstr(alloc, r->model_version);
    free_cstr(alloc, r->error_summary);
    if (r->metrics) {
        for (size_t i = 0; i < r->metrics_count; i++)
            free_cstr(alloc, r->metrics[i].name);
        alloc->free(alloc->ctx, r->metrics,
                    HU_EVALUATION_MAX_METRICS * sizeof(hu_evaluation_metric_t));
    }
    memset(r, 0, sizeof(*r));
}

/* ── report JSON serialise ──────────────────────────────────────────────── */

static hu_error_t append_string_kv(hu_json_buf_t *buf, const char *key, const char *val,
                                   bool *first) {
    if (!val)
        return HU_OK;
    if (!*first) {
        hu_error_t e = hu_json_buf_append_raw(buf, ",", 1);
        if (e != HU_OK)
            return e;
    }
    *first = false;
    hu_error_t e = hu_json_append_key(buf, key, strlen(key));
    if (e != HU_OK)
        return e;
    return hu_json_append_string(buf, val, strlen(val));
}

static hu_error_t append_int_kv(hu_json_buf_t *buf, const char *key, long long val, bool *first) {
    if (!*first) {
        hu_error_t e = hu_json_buf_append_raw(buf, ",", 1);
        if (e != HU_OK)
            return e;
    }
    *first = false;
    hu_error_t e = hu_json_append_key(buf, key, strlen(key));
    if (e != HU_OK)
        return e;
    char num[32];
    int n = snprintf(num, sizeof(num), "%lld", val);
    if (n < 0)
        return HU_ERR_INTERNAL;
    return hu_json_buf_append_raw(buf, num, (size_t)n);
}

static hu_error_t append_double_kv(hu_json_buf_t *buf, const char *key, double val, bool *first) {
    if (!*first) {
        hu_error_t e = hu_json_buf_append_raw(buf, ",", 1);
        if (e != HU_OK)
            return e;
    }
    *first = false;
    hu_error_t e = hu_json_append_key(buf, key, strlen(key));
    if (e != HU_OK)
        return e;
    char num[40];
    int n = snprintf(num, sizeof(num), "%.6f", val);
    if (n < 0)
        return HU_ERR_INTERNAL;
    return hu_json_buf_append_raw(buf, num, (size_t)n);
}

hu_error_t hu_evaluation_report_to_json(hu_allocator_t *alloc,
                                        const hu_evaluation_run_report_t *r, char **out_json,
                                        size_t *out_len) {
    if (!alloc || !r || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK)
        return err;

    err = hu_json_buf_append_raw(&buf, "{", 1);
    if (err != HU_OK)
        goto fail;

    bool first = true;
    err = append_string_kv(&buf, "suite_name", r->suite_name, &first);
    if (err != HU_OK)
        goto fail;
    err = append_string_kv(&buf, "model_version", r->model_version, &first);
    if (err != HU_OK)
        goto fail;
    err = append_int_kv(&buf, "started_at_ms", (long long)r->started_at_ms, &first);
    if (err != HU_OK)
        goto fail;
    err = append_int_kv(&buf, "finished_at_ms", (long long)r->finished_at_ms, &first);
    if (err != HU_OK)
        goto fail;
    err = append_int_kv(&buf, "prompts_total", (long long)r->prompts_total, &first);
    if (err != HU_OK)
        goto fail;
    err = append_int_kv(&buf, "prompts_passed", (long long)r->prompts_passed, &first);
    if (err != HU_OK)
        goto fail;
    err = append_int_kv(&buf, "prompts_failed", (long long)r->prompts_failed, &first);
    if (err != HU_OK)
        goto fail;
    err = append_string_kv(&buf, "error_summary", r->error_summary, &first);
    if (err != HU_OK)
        goto fail;

    if (!first) {
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;
    }
    first = false;
    err = hu_json_append_key(&buf, "metrics", 7);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, "[", 1);
    if (err != HU_OK)
        goto fail;
    for (size_t i = 0; i < r->metrics_count; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(&buf, ",", 1);
            if (err != HU_OK)
                goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "{", 1);
        if (err != HU_OK)
            goto fail;
        bool mfirst = true;
        err = append_string_kv(&buf, "name", r->metrics[i].name, &mfirst);
        if (err != HU_OK)
            goto fail;
        err = append_double_kv(&buf, "score", r->metrics[i].score, &mfirst);
        if (err != HU_OK)
            goto fail;
        err = append_int_kv(&buf, "sample_count", (long long)r->metrics[i].sample_count, &mfirst);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, "}", 1);
        if (err != HU_OK)
            goto fail;
    }
    err = hu_json_buf_append_raw(&buf, "]", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    /* hu_json_buf is not NUL-terminated; copy out into a sized buffer. */
    char *result = alloc->alloc(alloc->ctx, buf.len + 1);
    if (!result) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memcpy(result, buf.ptr, buf.len);
    result[buf.len] = '\0';
    *out_json = result;
    *out_len = buf.len;
    hu_json_buf_free(&buf);
    return HU_OK;

fail:
    hu_json_buf_free(&buf);
    return err;
}

hu_error_t hu_evaluation_report_from_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                          hu_evaluation_run_report_t *out) {
    if (!alloc || !json || json_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root)
        return err != HU_OK ? err : HU_ERR_PARSE;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    const char *suite = hu_json_get_string(root, "suite_name");
    const char *model = hu_json_get_string(root, "model_version");
    const char *errsum = hu_json_get_string(root, "error_summary");

    err = hu_evaluation_report_init(alloc, suite ? suite : "", out);
    if (err != HU_OK) {
        hu_json_free(alloc, root);
        return err;
    }
    if (model) {
        err = hu_evaluation_report_set_model(alloc, out, model);
        if (err != HU_OK)
            goto bail;
    }
    if (errsum) {
        err = hu_evaluation_report_set_error(alloc, out, errsum);
        if (err != HU_OK)
            goto bail;
    }
    out->started_at_ms = (int64_t)hu_json_get_number(root, "started_at_ms", 0.0);
    out->finished_at_ms = (int64_t)hu_json_get_number(root, "finished_at_ms", 0.0);
    out->prompts_total = (size_t)hu_json_get_number(root, "prompts_total", 0.0);
    out->prompts_passed = (size_t)hu_json_get_number(root, "prompts_passed", 0.0);
    out->prompts_failed = (size_t)hu_json_get_number(root, "prompts_failed", 0.0);

    hu_json_value_t *metrics_v = hu_json_object_get(root, "metrics");
    if (metrics_v && metrics_v->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < metrics_v->data.array.len; i++) {
            hu_json_value_t *m = metrics_v->data.array.items[i];
            if (!m || m->type != HU_JSON_OBJECT)
                continue;
            const char *name = hu_json_get_string(m, "name");
            if (!name)
                continue;
            double score = hu_json_get_number(m, "score", 0.0);
            size_t samples = (size_t)hu_json_get_number(m, "sample_count", 0.0);
            err = hu_evaluation_report_add_metric(alloc, out, name, score, samples);
            if (err != HU_OK)
                goto bail;
        }
    }

    hu_json_free(alloc, root);
    return HU_OK;

bail:
    hu_json_free(alloc, root);
    hu_evaluation_report_free(alloc, out);
    return err;
}
