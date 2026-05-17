/* Shared canned external-judge fixture loader (Tasks 7–8). */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/eval/eval_judge_external.h"

#include <stdio.h>
#include <string.h>

static hu_error_t load_fixture_file(hu_allocator_t *alloc, const char *path, char **out_buf,
                                    size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_buf = buf;
    *out_len = rd;
    return HU_OK;
}

hu_error_t hu_eval_judge_create_from_external_fixture(
    hu_allocator_t *alloc, const char *fixture_path, const char *judge_key,
    const char *judge_name, hu_eval_judge_external_t *out) {
    if (!alloc || !fixture_path || !judge_key || !judge_name || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t e = load_fixture_file(alloc, fixture_path, &raw, &raw_len);
    if (e != HU_OK)
        return e;

    hu_json_value_t *root = NULL;
    e = hu_json_parse(alloc, raw, raw_len, &root);
    alloc->free(alloc->ctx, raw, raw_len + 1);
    if (e != HU_OK)
        return e;

    hu_json_value_t *arr = hu_json_object_get(root, judge_key);
    if (!arr || arr->type != HU_JSON_ARRAY || arr->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_SUPPORTED;
    }

    size_t n = arr->data.array.len;
    hu_eval_judge_verdict_t *verdicts =
        (hu_eval_judge_verdict_t *)alloc->alloc(alloc->ctx, n * sizeof(*verdicts));
    if (!verdicts) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(verdicts, 0, n * sizeof(*verdicts));

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *row = arr->data.array.items[i];
        verdicts[i].prefer_a = (int)hu_json_get_number(row, "prefer_a", 0);
        verdicts[i].confidence = hu_json_get_number(row, "confidence", 0.5);
        const char *rat = hu_json_get_string(row, "rationale");
        if (rat) {
            size_t rlen = strlen(rat);
            char *copy = (char *)alloc->alloc(alloc->ctx, rlen + 1);
            if (!copy) {
                hu_json_free(alloc, root);
                alloc->free(alloc->ctx, verdicts, n * sizeof(*verdicts));
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(copy, rat, rlen + 1);
            verdicts[i].rationale = copy;
        }
    }

    hu_eval_judge_canned_config_t cfg = {.verdicts = verdicts, .n_verdicts = n};
    e = hu_eval_judge_create_canned(alloc, &cfg, out);
    hu_json_free(alloc, root);
    for (size_t i = 0; i < n; i++) {
        if (verdicts[i].rationale)
            alloc->free(alloc->ctx, (void *)verdicts[i].rationale,
                        strlen(verdicts[i].rationale) + 1);
    }
    alloc->free(alloc->ctx, verdicts, n * sizeof(*verdicts));
    (void)judge_name;
    return e;
}
