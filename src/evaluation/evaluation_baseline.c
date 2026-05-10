/* W16 — Baseline file load/save.
 *
 * The baseline is a flat JSON list of (suite_name, metric_name, score,
 * sample_count) entries. Stored under `docs/evaluation/baseline.json` and
 * tracked in git so regressions show up as PR diffs. Pure C; depends only on
 * the existing core/json.h.
 *
 * On-disk shape:
 *   {
 *     "entries": [
 *       {"suite": "locomo", "metric": "precision_at_1",
 *        "score": 0.80, "sample_count": 10},
 *       ...
 *     ]
 *   }
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

#define BASELINE_MAX_ENTRIES 256

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

void hu_evaluation_baseline_free(hu_allocator_t *alloc, hu_evaluation_baseline_t *b) {
    if (!alloc || !b)
        return;
    if (b->entries) {
        for (size_t i = 0; i < b->entries_count; i++) {
            free_cstr(alloc, b->entries[i].suite_name);
            free_cstr(alloc, b->entries[i].metric_name);
        }
        alloc->free(alloc->ctx, b->entries,
                    BASELINE_MAX_ENTRIES * sizeof(hu_evaluation_baseline_entry_t));
    }
    memset(b, 0, sizeof(*b));
}

hu_error_t hu_evaluation_baseline_load(hu_allocator_t *alloc, const char *json, size_t json_len,
                                       hu_evaluation_baseline_t *out) {
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

    out->entries =
        alloc->alloc(alloc->ctx, BASELINE_MAX_ENTRIES * sizeof(hu_evaluation_baseline_entry_t));
    if (!out->entries) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(out->entries, 0, BASELINE_MAX_ENTRIES * sizeof(hu_evaluation_baseline_entry_t));

    hu_json_value_t *entries_v = hu_json_object_get(root, "entries");
    if (!entries_v || entries_v->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_OK; /* zero entries is valid */
    }

    for (size_t i = 0; i < entries_v->data.array.len && out->entries_count < BASELINE_MAX_ENTRIES;
         i++) {
        hu_json_value_t *e = entries_v->data.array.items[i];
        if (!e || e->type != HU_JSON_OBJECT)
            continue;
        const char *suite = hu_json_get_string(e, "suite");
        const char *metric = hu_json_get_string(e, "metric");
        if (!suite || !metric)
            continue;
        hu_evaluation_baseline_entry_t *slot = &out->entries[out->entries_count];
        slot->suite_name = clone_cstr(alloc, suite);
        slot->metric_name = clone_cstr(alloc, metric);
        if (!slot->suite_name || !slot->metric_name) {
            free_cstr(alloc, slot->suite_name);
            free_cstr(alloc, slot->metric_name);
            slot->suite_name = NULL;
            slot->metric_name = NULL;
            hu_json_free(alloc, root);
            hu_evaluation_baseline_free(alloc, out);
            return HU_ERR_OUT_OF_MEMORY;
        }
        slot->score = hu_json_get_number(e, "score", 0.0);
        slot->sample_count = (size_t)hu_json_get_number(e, "sample_count", 0.0);
        out->entries_count++;
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_evaluation_baseline_save(hu_allocator_t *alloc,
                                       const hu_evaluation_baseline_t *baseline, char **out_json,
                                       size_t *out_len) {
    if (!alloc || !baseline || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK)
        return err;

    err = hu_json_buf_append_raw(&buf, "{\"entries\":[", 12);
    if (err != HU_OK)
        goto fail;

    for (size_t i = 0; i < baseline->entries_count; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(&buf, ",", 1);
            if (err != HU_OK)
                goto fail;
        }
        const hu_evaluation_baseline_entry_t *e = &baseline->entries[i];
        err = hu_json_buf_append_raw(&buf, "{", 1);
        if (err != HU_OK)
            goto fail;

        err = hu_json_append_key(&buf, "suite", 5);
        if (err != HU_OK)
            goto fail;
        err = hu_json_append_string(&buf, e->suite_name ? e->suite_name : "",
                                    e->suite_name ? strlen(e->suite_name) : 0);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;

        err = hu_json_append_key(&buf, "metric", 6);
        if (err != HU_OK)
            goto fail;
        err = hu_json_append_string(&buf, e->metric_name ? e->metric_name : "",
                                    e->metric_name ? strlen(e->metric_name) : 0);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;

        char num[40];
        int n = snprintf(num, sizeof(num), "\"score\":%.6f,\"sample_count\":%zu", e->score,
                         e->sample_count);
        if (n < 0 || (size_t)n >= sizeof(num)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, num, (size_t)n);
        if (err != HU_OK)
            goto fail;

        err = hu_json_buf_append_raw(&buf, "}", 1);
        if (err != HU_OK)
            goto fail;
    }

    err = hu_json_buf_append_raw(&buf, "]}", 2);
    if (err != HU_OK)
        goto fail;

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

bool hu_evaluation_baseline_lookup(const hu_evaluation_baseline_t *b, const char *suite_name,
                                   const char *metric_name, double *out_score) {
    if (!b || !suite_name || !metric_name)
        return false;
    for (size_t i = 0; i < b->entries_count; i++) {
        const hu_evaluation_baseline_entry_t *e = &b->entries[i];
        if (e->suite_name && e->metric_name && strcmp(e->suite_name, suite_name) == 0 &&
            strcmp(e->metric_name, metric_name) == 0) {
            if (out_score)
                *out_score = e->score;
            return true;
        }
    }
    return false;
}
