#include "human/eval/longmemeval.h"

#include "human/core/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lme_icontains(const char *hay, size_t hay_len, const char *needle) {
    if (!hay || !needle) {
        return 0;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return 1;
    }
    if (hay_len < nlen) {
        return 0;
    }
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + 32);
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b + 32);
            }
            if (a != b) {
                break;
            }
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}

static int lme_response_is_abstention(const char *resp, size_t resp_len) {
    if (!resp || resp_len == 0) {
        return 1;
    }
    /* Trim leading whitespace. */
    size_t i = 0;
    while (i < resp_len && (resp[i] == ' ' || resp[i] == '\t' || resp[i] == '\n')) {
        i++;
    }
    size_t remain = resp_len - i;
    const char *p = resp + i;
    if (remain == 0) {
        return 1;
    }
    static const char *const ABSTAIN[] = {
        "i don't know",        "i do not know",     "no record", "no idea",
        "can't recall",        "cannot recall",     "unknown",   "i'm not sure",
        "no information",      "no memory of that", "not sure", NULL,
    };
    for (size_t k = 0; ABSTAIN[k]; k++) {
        if (lme_icontains(p, remain, ABSTAIN[k])) {
            return 1;
        }
    }
    return 0;
}

hu_error_t hu_longmemeval_score_item(const char *category, const char *response, size_t response_len,
                                     const char *const *keywords, size_t keyword_count,
                                     hu_longmemeval_score_t *out) {
    if (!out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    int abstain = lme_response_is_abstention(response, response_len);
    out->abstained = (bool)abstain;

    if (category && strcmp(category, "abstention") == 0) {
        out->score = abstain ? 100 : 0;
        out->keywords_total = keyword_count;
        out->keywords_seen = abstain ? keyword_count : 0;
        return HU_OK;
    }

    if (!keywords || keyword_count == 0) {
        /* No keywords — treat any non-empty grounded response as 50, abstain
         * as 0 (we have nothing to verify). */
        out->score = abstain ? 0 : 50;
        return HU_OK;
    }
    size_t hits = 0;
    for (size_t i = 0; i < keyword_count; i++) {
        const char *kw = keywords[i];
        if (!kw) {
            continue;
        }
        if (lme_icontains(response, response_len, kw)) {
            hits++;
        }
    }
    out->keywords_total = keyword_count;
    out->keywords_seen = hits;
    int recall = (int)((hits * 100) / keyword_count);
    out->score = recall;
    return HU_OK;
}

static char *lme_read_entire(const char *path, hu_allocator_t *alloc, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(2 * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) {
        *out_len = rd;
    }
    return buf;
}

hu_error_t hu_longmemeval_run_pack_self_test(hu_allocator_t *alloc, const char *json_path,
                                             unsigned *out_total, unsigned *out_passed,
                                             int *out_mean_score) {
    if (!alloc || !json_path) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (out_total) {
        *out_total = 0;
    }
    if (out_passed) {
        *out_passed = 0;
    }
    if (out_mean_score) {
        *out_mean_score = 0;
    }

    size_t buflen = 0;
    char *buf = lme_read_entire(json_path, alloc, &buflen);
    if (!buf) {
        return HU_ERR_NOT_FOUND;
    }
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, buflen, &root);
    alloc->free(alloc->ctx, buf, buflen + 1);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root) {
            hu_json_free(alloc, root);
        }
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;
    }
    hu_json_value_t *items = hu_json_object_get(root, "items");
    if (!items || items->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_ERR_JSON_PARSE;
    }
    unsigned total = 0;
    unsigned passed = 0;
    long sum_score = 0;
    /* Local stack scratch for keyword pointers; cap to 64 per item. */
    enum { LME_MAX_KW = 64 };
    const char *kw_ptrs[LME_MAX_KW];
    for (size_t i = 0; i < items->data.array.len; i++) {
        const hu_json_value_t *it = items->data.array.items[i];
        if (!it || it->type != HU_JSON_OBJECT) {
            continue;
        }
        const char *category = hu_json_get_string(it, "category");
        const char *answer = hu_json_get_string(it, "candidate_answer");
        const hu_json_value_t *kws = hu_json_object_get(it, "keywords");
        if (!answer) {
            continue;
        }
        size_t kc = 0;
        if (kws && kws->type == HU_JSON_ARRAY) {
            for (size_t j = 0; j < kws->data.array.len && kc < LME_MAX_KW; j++) {
                const hu_json_value_t *kv = kws->data.array.items[j];
                if (kv && kv->type == HU_JSON_STRING && kv->data.string.ptr) {
                    kw_ptrs[kc++] = kv->data.string.ptr;
                }
            }
        }
        hu_longmemeval_score_t s = {0};
        hu_longmemeval_score_item(category, answer, strlen(answer), kw_ptrs, kc, &s);
        total++;
        sum_score += s.score;
        if (s.score >= 80) {
            passed++;
        }
    }
    hu_json_free(alloc, root);
    if (out_total) {
        *out_total = total;
    }
    if (out_passed) {
        *out_passed = passed;
    }
    if (out_mean_score) {
        *out_mean_score = total > 0 ? (int)(sum_score / (long)total) : 0;
    }
    return HU_OK;
}
