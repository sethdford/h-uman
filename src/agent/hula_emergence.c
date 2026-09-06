#include "human/agent/hula_emergence.h"
#include "human/core/json.h"
#include "human/core/paths.h"
#include "human/core/string.h"
#include "human/platform.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define HU_HULA_EMERGENCE_MAX_BUCKETS 128
#define HU_HULA_EMERGENCE_MAX_TOOLS   256

typedef struct {
    char *key;
    size_t count;
} emergence_bucket_t;

static void default_trace_dir(char *buf, size_t cap) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    const char *p = getenv("USERPROFILE");
    if (!p || !p[0]) {
        buf[0] = '\0';
        return;
    }
    (void)hu_paths_state(buf, cap, "hula_traces");
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        buf[0] = '\0';
        return;
    }
    (void)hu_paths_state(buf, cap, "hula_traces");
#endif
}

/* Create ~/.human then ~/.human/hula_traces (or USERPROFILE equivalent). Single dir when explicit.
 */
static hu_error_t hula_trace_prepare_dir(const char *dir_buf, bool is_default_path) {
    if (is_default_path) {
        const char *slash = strrchr(dir_buf, '/');
        if (!slash || slash == dir_buf)
            return HU_ERR_INVALID_ARGUMENT;
        size_t plen = (size_t)(slash - dir_buf);
        if (plen >= 512)
            return HU_ERR_INVALID_ARGUMENT;
        char parent[512];
        memcpy(parent, dir_buf, plen);
        parent[plen] = '\0';
        if (hu_platform_mkdir(parent, 0755) != 0 && errno != EEXIST)
            return HU_ERR_IO;
    }
    if (hu_platform_mkdir(dir_buf, 0755) != 0 && errno != EEXIST)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_hula_trace_persist(hu_allocator_t *alloc, const char *trace_dir,
                                 const char *trace_json, size_t trace_json_len,
                                 const char *program_name, size_t program_name_len, bool success,
                                 const char *program_json, size_t program_json_len,
                                 const char *program_source, size_t program_source_len) {
#if defined(HU_IS_TEST)
    if (!trace_dir)
        return HU_OK;
#endif

#if !((defined(__unix__) || defined(__APPLE__)) || (defined(_WIN32) && !defined(__CYGWIN__)))
    (void)alloc;
    (void)trace_dir;
    (void)trace_json;
    (void)trace_json_len;
    (void)program_name;
    (void)program_name_len;
    (void)success;
    (void)program_json;
    (void)program_json_len;
    (void)program_source;
    (void)program_source_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char dir_buf[512];
    bool is_default = false;
    if (trace_dir && trace_dir[0]) {
        size_t dl = strlen(trace_dir);
        if (dl >= sizeof(dir_buf))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(dir_buf, trace_dir, dl + 1);
    } else {
        default_trace_dir(dir_buf, sizeof(dir_buf));
        is_default = true;
    }
    if (!dir_buf[0])
        return HU_ERR_NOT_FOUND;
    if (hula_trace_prepare_dir(dir_buf, is_default) != HU_OK)
        return HU_ERR_IO;

    time_t t = time(NULL);
    unsigned long salt = (unsigned long)t ^ (unsigned long)(uintptr_t)trace_json;
    char path[640];
    const char *pn = program_name && program_name_len > 0 ? program_name : "trace";
    int n =
        snprintf(path, sizeof(path), "%s/%lx_%.*s.json", dir_buf, salt, (int)program_name_len, pn);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, root, "version", hu_json_number_new(alloc, 1));
    hu_json_object_set(alloc, root, "success", hu_json_bool_new(alloc, success));
    hu_json_object_set(alloc, root, "ts", hu_json_number_new(alloc, (double)t));
    if (program_name && program_name_len > 0)
        hu_json_object_set(alloc, root, "program_name",
                           hu_json_string_new(alloc, program_name, program_name_len));

    hu_json_value_t *trace_arr = NULL;
    if (trace_json && trace_json_len > 0 &&
        hu_json_parse(alloc, trace_json, trace_json_len, &trace_arr) != HU_OK)
        trace_arr = NULL;
    if (!trace_arr)
        trace_arr = hu_json_array_new(alloc);
    hu_json_object_set(alloc, root, "trace", trace_arr);

    if (program_json && program_json_len > 0) {
        hu_json_value_t *prog_val = NULL;
        if (hu_json_parse(alloc, program_json, program_json_len, &prog_val) == HU_OK && prog_val)
            hu_json_object_set(alloc, root, "program", prog_val);
    }
    if (program_source && program_source_len > 0)
        hu_json_object_set(alloc, root, "program_source",
                           hu_json_string_new(alloc, program_source, program_source_len));

    if (program_source && program_source_len > 0)
        hu_json_object_set(alloc, root, "program_source",
                           hu_json_string_new(alloc, program_source, program_source_len));

    char *file_body = NULL;
    size_t file_len = 0;
    hu_error_t err = hu_json_stringify(alloc, root, &file_body, &file_len);
    hu_json_free(alloc, root);
    if (err != HU_OK || !file_body)
        return err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;

    FILE *f = fopen(path, "wb");
    if (!f) {
        hu_str_free(alloc, file_body);
        return HU_ERR_IO;
    }
    fwrite(file_body, 1, file_len, f);
    fclose(f);
    hu_str_free(alloc, file_body);
    return HU_OK;
#endif /* platform */
}

static int bucket_find_or_add(emergence_bucket_t *buckets, size_t *bucket_count, const char *key) {
    for (size_t i = 0; i < *bucket_count; i++) {
        if (buckets[i].key && strcmp(buckets[i].key, key) == 0)
            return (int)i;
    }
    if (*bucket_count >= HU_HULA_EMERGENCE_MAX_BUCKETS)
        return -1;
    size_t idx = *bucket_count;
    buckets[idx].key = strdup(key);
    if (!buckets[idx].key)
        return -1;
    buckets[idx].count = 0;
    (*bucket_count)++;
    return (int)idx;
}

static void buckets_free(emergence_bucket_t *buckets, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(buckets[i].key);
}

static hu_error_t extract_tools_from_trace_array(hu_allocator_t *alloc, hu_json_value_t *arr,
                                                 char tools[][64], size_t *tool_count) {
    *tool_count = 0;
    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_OK;
    for (size_t i = 0; i < arr->data.array.len && *tool_count < HU_HULA_EMERGENCE_MAX_TOOLS; i++) {
        hu_json_value_t *el = arr->data.array.items[i];
        if (!el || el->type != HU_JSON_OBJECT)
            continue;
        const char *op = hu_json_get_string(el, "op");
        if (!op || strcmp(op, "call") != 0)
            continue;
        const char *tn = hu_json_get_string(el, "tool");
        if (!tn || !tn[0])
            continue;
        size_t tl = strlen(tn);
        if (tl >= 64)
            continue;
        memcpy(tools[*tool_count], tn, tl + 1);
        (*tool_count)++;
    }
    (void)alloc;
    return HU_OK;
}

hu_error_t hu_hula_emergence_scan(hu_allocator_t *alloc, const char *trace_dir, size_t ngram_len,
                                  size_t min_occurrences, char ***out_patterns,
                                  size_t *out_pattern_count, size_t **out_freqs) {
    if (!alloc || !trace_dir || !out_patterns || !out_pattern_count || !out_freqs)
        return HU_ERR_INVALID_ARGUMENT;
    *out_patterns = NULL;
    *out_pattern_count = 0;
    *out_freqs = NULL;
    if (ngram_len == 0 || ngram_len > 8)
        return HU_ERR_INVALID_ARGUMENT;

    emergence_bucket_t buckets[HU_HULA_EMERGENCE_MAX_BUCKETS];
    memset(buckets, 0, sizeof(buckets));
    size_t bc = 0;

#if defined(__unix__) || defined(__APPLE__)
    DIR *d = opendir(trace_dir);
    if (!d)
        return HU_ERR_NOT_FOUND;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 6 || strcmp(de->d_name + nl - 5, ".json") != 0)
            continue;
        char path[768];
        if (snprintf(path, sizeof(path), "%s/%s", trace_dir, de->d_name) >= (int)sizeof(path)) {
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 1 << 20) {
            fclose(f);
            continue;
        }
        char *buf = alloc->alloc(alloc->ctx, (size_t)sz + 1);
        if (!buf) {
            fclose(f);
            closedir(d);
            buckets_free(buckets, bc);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[rd] = '\0';

        hu_json_value_t *root = NULL;
        if (hu_json_parse(alloc, buf, rd, &root) != HU_OK || !root) {
            alloc->free(alloc->ctx, buf, (size_t)sz + 1);
            continue;
        }
        hu_json_value_t *tr = hu_json_object_get(root, "trace");
        char tools[HU_HULA_EMERGENCE_MAX_TOOLS][64];
        size_t tc = 0;
        (void)extract_tools_from_trace_array(alloc, tr, tools, &tc);
        hu_json_free(alloc, root);
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);

        if (tc < ngram_len)
            continue;
        for (size_t i = 0; i + ngram_len <= tc; i++) {
            char key[512];
            size_t kp = 0;
            for (size_t j = 0; j < ngram_len; j++) {
                if (j > 0)
                    key[kp++] = '|';
                size_t tl = strlen(tools[i + j]);
                if (kp + tl + 2 >= sizeof(key))
                    break;
                memcpy(key + kp, tools[i + j], tl);
                kp += tl;
            }
            key[kp] = '\0';
            int bi = bucket_find_or_add(buckets, &bc, key);
            if (bi >= 0)
                buckets[(size_t)bi].count++;
        }
    }
    closedir(d);
#else
    (void)min_occurrences;
    return HU_ERR_NOT_SUPPORTED;
#endif /* unix */

    size_t out_n = 0;
    for (size_t i = 0; i < bc; i++) {
        if (buckets[i].count >= min_occurrences)
            out_n++;
    }
    if (out_n == 0) {
        buckets_free(buckets, bc);
        return HU_OK;
    }

    char **pat = alloc->alloc(alloc->ctx, out_n * sizeof(char *));
    size_t *fq = alloc->alloc(alloc->ctx, out_n * sizeof(size_t));
    if (!pat || !fq) {
        if (pat)
            alloc->free(alloc->ctx, pat, out_n * sizeof(char *));
        if (fq)
            alloc->free(alloc->ctx, fq, out_n * sizeof(size_t));
        buckets_free(buckets, bc);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t w = 0;
    for (size_t i = 0; i < bc; i++) {
        if (buckets[i].count < min_occurrences)
            continue;
        pat[w] = hu_strdup(alloc, buckets[i].key);
        if (!pat[w]) {
            for (size_t k = 0; k < w; k++)
                alloc->free(alloc->ctx, pat[k], strlen(pat[k]) + 1);
            alloc->free(alloc->ctx, pat, out_n * sizeof(char *));
            alloc->free(alloc->ctx, fq, out_n * sizeof(size_t));
            buckets_free(buckets, bc);
            return HU_ERR_OUT_OF_MEMORY;
        }
        fq[w] = buckets[i].count;
        w++;
    }
    buckets_free(buckets, bc);
    *out_patterns = pat;
    *out_freqs = fq;
    *out_pattern_count = w;
    return HU_OK;
}

hu_error_t hu_hula_emergence_promote(hu_allocator_t *alloc, const char *skills_dir,
                                     const char *pattern, size_t pattern_len,
                                     const char *skill_name, size_t skill_name_len) {
    if (!alloc || !pattern || pattern_len == 0 || !skill_name || skill_name_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    char base[512];
    if (skills_dir && skills_dir[0]) {
        if (strlen(skills_dir) >= sizeof(base))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(base, skills_dir, strlen(skills_dir) + 1);
    } else {
        const char *home = getenv("HOME");
        if (!home)
            return HU_ERR_NOT_FOUND;
        if (hu_paths_state(base, sizeof(base), "skills") >= (int)sizeof(base))
            return HU_ERR_INVALID_ARGUMENT;
    }
    if (hula_trace_prepare_dir(base, true) != HU_OK)
        return HU_ERR_IO;

    /* Build HuLa program: seq of calls with empty args */
    hu_json_value_t *calls = hu_json_array_new(alloc);
    if (!calls)
        return HU_ERR_OUT_OF_MEMORY;

    const char *p = pattern;
    const char *end = pattern + pattern_len;
    size_t idx = 0;
    while (p < end) {
        const char *sep = memchr(p, '|', (size_t)(end - p));
        const char *tok_end = sep ? sep : end;
        size_t tl = (size_t)(tok_end - p);
        if (tl > 0 && tl < 128) {
            char tbuf[128];
            memcpy(tbuf, p, tl);
            tbuf[tl] = '\0';
            hu_json_value_t *cn = hu_json_object_new(alloc);
            if (cn) {
                char nid[32];
                (void)snprintf(nid, sizeof(nid), "s%zu", idx);
                hu_json_object_set(alloc, cn, "op", hu_json_string_new(alloc, "call", 4));
                hu_json_object_set(alloc, cn, "id", hu_json_string_new(alloc, nid, strlen(nid)));
                hu_json_object_set(alloc, cn, "tool", hu_json_string_new(alloc, tbuf, tl));
                hu_json_object_set(alloc, cn, "args", hu_json_object_new(alloc));
                hu_json_array_push(alloc, calls, cn);
                idx++;
            }
        }
        p = sep ? sep + 1 : end;
    }

    hu_json_value_t *seq = hu_json_object_new(alloc);
    if (!seq) {
        hu_json_free(alloc, calls);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, seq, "op", hu_json_string_new(alloc, "seq", 3));
    hu_json_object_set(alloc, seq, "id", hu_json_string_new(alloc, "root", 4));
    hu_json_object_set(alloc, seq, "children", calls);

    hu_json_value_t *prog = hu_json_object_new(alloc);
    if (!prog) {
        hu_json_free(alloc, seq);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, prog, "name", hu_json_string_new(alloc, skill_name, skill_name_len));
    hu_json_object_set(alloc, prog, "version", hu_json_number_new(alloc, 1));
    hu_json_object_set(alloc, prog, "root", seq);

    char *hula_json = NULL;
    size_t hula_len = 0;
    hu_error_t err = hu_json_stringify(alloc, prog, &hula_json, &hula_len);
    hu_json_free(alloc, prog);
    if (err != HU_OK || !hula_json)
        return err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *manifest = hu_json_object_new(alloc);
    if (!manifest) {
        hu_str_free(alloc, hula_json);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, manifest, "name",
                       hu_json_string_new(alloc, skill_name, skill_name_len));
    hu_json_object_set(
        alloc, manifest, "description",
        hu_json_string_new(alloc, "Emergent HuLa pattern from execution traces",
                           sizeof("Emergent HuLa pattern from execution traces") - 1));
    hu_json_object_set(alloc, manifest, "enabled", hu_json_bool_new(alloc, true));
    hu_json_object_set(alloc, manifest, "parameters", hu_json_object_new(alloc));

    char *mf = NULL;
    size_t mf_len = 0;
    err = hu_json_stringify(alloc, manifest, &mf, &mf_len);
    hu_json_free(alloc, manifest);
    if (err != HU_OK || !mf) {
        hu_str_free(alloc, hula_json);
        return err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;
    }

    char skill_path[640];
    if (snprintf(skill_path, sizeof(skill_path), "%s/%.*s.skill.json", base, (int)skill_name_len,
                 skill_name) >= (int)sizeof(skill_path)) {
        hu_str_free(alloc, hula_json);
        hu_str_free(alloc, mf);
        return HU_ERR_INVALID_ARGUMENT;
    }

    FILE *sf = fopen(skill_path, "wb");
    if (!sf) {
        hu_str_free(alloc, hula_json);
        hu_str_free(alloc, mf);
        return HU_ERR_IO;
    }
    fwrite(mf, 1, mf_len, sf);
    fclose(sf);
    hu_str_free(alloc, mf);

    char md_path[640];
    if (snprintf(md_path, sizeof(md_path), "%s/%.*s_HULA.md", base, (int)skill_name_len,
                 skill_name) >= (int)sizeof(md_path)) {
        hu_str_free(alloc, hula_json);
        return HU_ERR_INVALID_ARGUMENT;
    }
    FILE *mf2 = fopen(md_path, "wb");
    if (mf2) {
        fputs("# HuLa program (emerged)\n\nRun with: `human hula run '<json>'` or embed in "
              "agent.\n\n```json\n",
              mf2);
        fwrite(hula_json, 1, hula_len, mf2);
        fputs("\n```\n", mf2);
        fclose(mf2);
    }
    hu_str_free(alloc, hula_json);
    return HU_OK;
}
