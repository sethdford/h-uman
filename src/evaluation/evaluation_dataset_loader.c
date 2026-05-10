/* W16 — Real-corpus loader implementation. */

#include "evaluation_dataset_loader.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Default base dir — overridable via HU_EVAL_DATA_DIR env var. */
static const char *eval_data_dir(void) {
    const char *env = getenv("HU_EVAL_DATA_DIR");
    if (env && *env)
        return env;
    static char fallback[512];
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    snprintf(fallback, sizeof(fallback), "%s/.human/eval-datasets", home);
    return fallback;
}

bool hu_eval_dataset_resolve_path(const char *suite, char *out_buf, size_t out_cap) {
    if (!suite || !out_buf || out_cap == 0)
        return false;
    const char *dir = eval_data_dir();
    int n = snprintf(out_buf, out_cap, "%s/%s.json", dir, suite);
    return n > 0 && (size_t)n < out_cap;
}

/* Slurp the entire file into a heap buffer the caller frees. Returns
 * NULL when the file doesn't exist (HU_ERR_NOT_FOUND) or can't be read
 * (HU_ERR_IO). */
static hu_error_t read_file(hu_allocator_t *alloc, const char *path, char **out_buf,
                             size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;
    struct stat st;
    if (stat(path, &st) != 0)
        return HU_ERR_NOT_FOUND;
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        fclose(f);
        return HU_ERR_IO;
    }
    buf[sz] = '\0';
    fclose(f);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return HU_OK;
}

/* Duplicate a (possibly NULL) string into a malloc owned by alloc. */
static char *dup_or_null(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *out = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* Total byte cost of a single LoCoMo item — used for symmetric free. */
static size_t locomo_item_owned_bytes(const hu_eval_locomo_item_t *it) {
    size_t total = 0;
    if (it->fact_id)
        total += strlen(it->fact_id) + 1;
    if (it->fact)
        total += strlen(it->fact) + 1;
    if (it->query)
        total += strlen(it->query) + 1;
    if (it->expected_id)
        total += strlen(it->expected_id) + 1;
    return total;
}

void hu_eval_locomo_free(hu_allocator_t *alloc, hu_eval_locomo_dataset_t *ds) {
    if (!alloc || !ds || !ds->items)
        return;
    for (size_t i = 0; i < ds->count; i++) {
        hu_eval_locomo_item_t *it = &ds->items[i];
        if (it->fact_id)
            alloc->free(alloc->ctx, it->fact_id, strlen(it->fact_id) + 1);
        if (it->fact)
            alloc->free(alloc->ctx, it->fact, strlen(it->fact) + 1);
        if (it->query)
            alloc->free(alloc->ctx, it->query, strlen(it->query) + 1);
        if (it->expected_id)
            alloc->free(alloc->ctx, it->expected_id, strlen(it->expected_id) + 1);
    }
    alloc->free(alloc->ctx, ds->items, ds->count * sizeof(hu_eval_locomo_item_t));
    ds->items = NULL;
    ds->count = 0;
    (void)locomo_item_owned_bytes; /* used in tests; keep linker happy */
}

hu_error_t hu_eval_locomo_load(hu_allocator_t *alloc, hu_eval_locomo_dataset_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->items = NULL;
    out->count = 0;

    char path[768];
    if (!hu_eval_dataset_resolve_path("locomo", path, sizeof(path)))
        return HU_ERR_INVALID_ARGUMENT;

    char *buf = NULL;
    size_t buflen = 0;
    hu_error_t err = read_file(alloc, path, &buf, &buflen);
    if (err != HU_OK)
        return err; /* HU_ERR_NOT_FOUND on missing — caller falls back. */

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, buf, buflen, &root);
    alloc->free(alloc->ctx, buf, buflen + 1);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_TOOL_VALIDATION;
    }

    hu_json_value_t *items = hu_json_object_get(root, "items");
    if (!items || items->type != HU_JSON_ARRAY || items->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_TOOL_VALIDATION;
    }

    size_t n = items->data.array.len;
    hu_eval_locomo_item_t *list = (hu_eval_locomo_item_t *)alloc->alloc(
        alloc->ctx, n * sizeof(hu_eval_locomo_item_t));
    if (!list) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(list, 0, n * sizeof(hu_eval_locomo_item_t));

    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *e = items->data.array.items[i];
        if (!e || e->type != HU_JSON_OBJECT)
            continue;
        const char *fid = hu_json_get_string(e, "fact_id");
        const char *fact = hu_json_get_string(e, "fact");
        const char *q = hu_json_get_string(e, "query");
        const char *eid = hu_json_get_string(e, "expected_id");
        /* All four fields are mandatory — skip malformed rows rather
         * than fail the whole load, so a partially-broken file still
         * yields a runnable subset. */
        if (!fid || !fact || !q || !eid)
            continue;
        hu_eval_locomo_item_t *it = &list[kept];
        it->fact_id = dup_or_null(alloc, fid);
        it->fact = dup_or_null(alloc, fact);
        it->query = dup_or_null(alloc, q);
        it->expected_id = dup_or_null(alloc, eid);
        if (!it->fact_id || !it->fact || !it->query || !it->expected_id) {
            /* Roll back any partial allocation for this row, then free the
             * already-stored prior rows and the (still original-sized)
             * array. We can't go through hu_eval_locomo_free because the
             * array is sized for `n` slots, not `kept`. */
            if (it->fact_id)
                alloc->free(alloc->ctx, it->fact_id, strlen(it->fact_id) + 1);
            if (it->fact)
                alloc->free(alloc->ctx, it->fact, strlen(it->fact) + 1);
            if (it->query)
                alloc->free(alloc->ctx, it->query, strlen(it->query) + 1);
            if (it->expected_id)
                alloc->free(alloc->ctx, it->expected_id, strlen(it->expected_id) + 1);
            memset(it, 0, sizeof(*it));
            for (size_t k = 0; k < kept; k++) {
                hu_eval_locomo_item_t *prev = &list[k];
                if (prev->fact_id)
                    alloc->free(alloc->ctx, prev->fact_id, strlen(prev->fact_id) + 1);
                if (prev->fact)
                    alloc->free(alloc->ctx, prev->fact, strlen(prev->fact) + 1);
                if (prev->query)
                    alloc->free(alloc->ctx, prev->query, strlen(prev->query) + 1);
                if (prev->expected_id)
                    alloc->free(alloc->ctx, prev->expected_id, strlen(prev->expected_id) + 1);
            }
            alloc->free(alloc->ctx, list, n * sizeof(hu_eval_locomo_item_t));
            hu_json_free(alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        kept++;
    }

    hu_json_free(alloc, root);

    if (kept == 0) {
        alloc->free(alloc->ctx, list, n * sizeof(hu_eval_locomo_item_t));
        return HU_ERR_TOOL_VALIDATION;
    }
    /* If we skipped rows the trailing slots are zeroed; resize to fit
     * exactly so the count and the allocation match (so free is symmetric). */
    if (kept < n) {
        hu_eval_locomo_item_t *tight = (hu_eval_locomo_item_t *)alloc->alloc(
            alloc->ctx, kept * sizeof(hu_eval_locomo_item_t));
        if (tight) {
            memcpy(tight, list, kept * sizeof(hu_eval_locomo_item_t));
            alloc->free(alloc->ctx, list, n * sizeof(hu_eval_locomo_item_t));
            list = tight;
        }
        /* If the shrink-realloc fails we keep the larger allocation;
         * count records the live entries and free still walks just
         * those. The over-allocated tail is zero-filled so traversing
         * beyond `count` would be a caller bug, not ours. */
    }
    out->items = list;
    out->count = kept;
    return HU_OK;
}
