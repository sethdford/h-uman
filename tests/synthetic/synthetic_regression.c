#include "synthetic_harness.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

sc_error_t sc_synth_regression_save(sc_allocator_t *alloc, const char *dir,
                                    const sc_synth_test_case_t *tc) {
    if (!dir || !tc)
        return SC_ERR_INVALID_ARGUMENT;
    mkdir(dir, 0755);
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    if (tc->name)
        sc_json_object_set(alloc, obj, "name",
                           sc_json_string_new(alloc, tc->name, strlen(tc->name)));
    if (tc->category)
        sc_json_object_set(alloc, obj, "category",
                           sc_json_string_new(alloc, tc->category, strlen(tc->category)));
    if (tc->input_json) {
        sc_json_value_t *iv = NULL;
        if (sc_json_parse(alloc, tc->input_json, strlen(tc->input_json), &iv) == SC_OK)
            sc_json_object_set(alloc, obj, "input", iv);
    }
    if (tc->actual_output)
        sc_json_object_set(alloc, obj, "actual",
                           sc_json_string_new(alloc, tc->actual_output, strlen(tc->actual_output)));
    if (tc->verdict_reason)
        sc_json_object_set(
            alloc, obj, "reason",
            sc_json_string_new(alloc, tc->verdict_reason, strlen(tc->verdict_reason)));
    const char *vs = sc_synth_verdict_str(tc->verdict);
    sc_json_object_set(alloc, obj, "verdict", sc_json_string_new(alloc, vs, strlen(vs)));
    sc_json_object_set(alloc, obj, "latency_ms", sc_json_number_new(alloc, tc->latency_ms));

    char *js = NULL;
    size_t jl = 0;
    sc_error_t err = sc_json_stringify(alloc, obj, &js, &jl);
    sc_json_free(alloc, obj);
    if (err != SC_OK)
        return err;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%ld.json", dir, tc->name ? tc->name : "test",
             (long)time(NULL));
    FILE *f = fopen(path, "w");
    if (!f) {
        alloc->free(alloc->ctx, js, jl);
        return SC_ERR_IO;
    }
    fwrite(js, 1, jl, f);
    fclose(f);
    alloc->free(alloc->ctx, js, jl);
    SC_SYNTH_LOG("  saved: %s", path);
    return SC_OK;
}

sc_error_t sc_synth_regression_load(sc_allocator_t *alloc, const char *dir,
                                    sc_synth_test_case_t **tests_out, size_t *count_out) {
    if (!dir || !tests_out || !count_out)
        return SC_ERR_INVALID_ARGUMENT;
    DIR *d = opendir(dir);
    if (!d)
        return SC_ERR_NOT_FOUND;
    size_t cap = 32, n = 0;
    sc_synth_test_case_t *tests =
        (sc_synth_test_case_t *)alloc->alloc(alloc->ctx, cap * sizeof(*tests));
    if (!tests) {
        closedir(d);
        return SC_ERR_OUT_OF_MEMORY;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 5 || strcmp(ent->d_name + nl - 5, ".json") != 0)
            continue;
        char fp[512];
        snprintf(fp, sizeof(fp), "%s/%s", dir, ent->d_name);
        FILE *f = fopen(fp, "r");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long fs = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fs <= 0 || fs > 1024 * 1024) {
            fclose(f);
            continue;
        }
        char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)fs + 1);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t rd = fread(buf, 1, (size_t)fs, f);
        fclose(f);
        buf[rd] = '\0';
        sc_json_value_t *rj = NULL;
        if (sc_json_parse(alloc, buf, rd, &rj) != SC_OK) {
            alloc->free(alloc->ctx, buf, (size_t)fs + 1);
            continue;
        }
        if (n >= cap) {
            size_t nc = cap * 2;
            sc_synth_test_case_t *nt =
                (sc_synth_test_case_t *)alloc->alloc(alloc->ctx, nc * sizeof(*nt));
            if (!nt) {
                sc_json_free(alloc, rj);
                alloc->free(alloc->ctx, buf, (size_t)fs + 1);
                break;
            }
            memcpy(nt, tests, n * sizeof(*nt));
            alloc->free(alloc->ctx, tests, cap * sizeof(*tests));
            tests = nt;
            cap = nc;
        }
        sc_synth_test_case_t *tc = &tests[n];
        memset(tc, 0, sizeof(*tc));
        const char *nm = sc_json_get_string(rj, "name");
        const char *cat = sc_json_get_string(rj, "category");
        const char *reason = sc_json_get_string(rj, "reason");
        tc->name = nm ? sc_synth_strdup(alloc, nm, strlen(nm)) : NULL;
        tc->category = cat ? sc_synth_strdup(alloc, cat, strlen(cat)) : NULL;
        tc->verdict_reason = reason ? sc_synth_strdup(alloc, reason, strlen(reason)) : NULL;
        sc_json_value_t *iv = sc_json_object_get(rj, "input");
        if (iv) {
            char *s = NULL;
            size_t sl = 0;
            (void)sc_json_stringify(alloc, iv, &s, &sl);
            tc->input_json = s;
        }
        n++;
        sc_json_free(alloc, rj);
        alloc->free(alloc->ctx, buf, (size_t)fs + 1);
    }
    closedir(d);
    *tests_out = tests;
    *count_out = n;
    return SC_OK;
}

void sc_synth_regression_free_tests(sc_allocator_t *alloc, sc_synth_test_case_t *tests,
                                    size_t count) {
    if (!tests)
        return;
    for (size_t i = 0; i < count; i++)
        sc_synth_test_case_free(alloc, &tests[i]);
    alloc->free(alloc->ctx, tests, count * sizeof(*tests));
}
