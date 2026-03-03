/* JSON API benchmarks: parse, access, build. */
#include "bench.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_ITERATIONS 10000

static char *make_medium_json(sc_allocator_t *alloc, size_t num_keys) {
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, alloc);
    sc_json_buf_append_raw(&buf, "{", 1);
    for (size_t i = 0; i < num_keys; i++) {
        if (i > 0)
            sc_json_buf_append_raw(&buf, ",", 1);
        char key[32];
        int n = snprintf(key, sizeof(key), "\"key_%zu\"", i);
        sc_json_buf_append_raw(&buf, key, (size_t)n);
        sc_json_buf_append_raw(&buf, ":", 1);
        if (i % 3 == 0) {
            char val[32];
            n = snprintf(val, sizeof(val), "\"value_%zu\"", i);
            sc_json_buf_append_raw(&buf, val, (size_t)n);
        } else if (i % 3 == 1) {
            char val[16];
            n = snprintf(val, sizeof(val), "%zu", i);
            sc_json_buf_append_raw(&buf, val, (size_t)n);
        } else {
            sc_json_buf_append_raw(&buf, "true", 4);
        }
    }
    sc_json_buf_append_raw(&buf, "}", 1);
    char *out = buf.ptr ? strdup(buf.ptr) : NULL;
    sc_json_buf_free(&buf);
    return out;
}

void run_bench_json(void) {
    sc_allocator_t alloc = sc_system_allocator();

    /* Parse: medium object with 100 keys */
    char *json100 = make_medium_json(&alloc, 100);
    if (!json100) {
        fprintf(stderr, "bench_json: failed to build medium JSON\n");
        return;
    }
    size_t json100_len = strlen(json100);

    BENCH_RUN("json_parse_100keys", JSON_ITERATIONS, {
        sc_json_value_t *val = NULL;
        sc_json_parse(&alloc, json100, json100_len, &val);
        if (val)
            sc_json_free(&alloc, val);
    });

    /* Access nested fields: parse once, then access */
    sc_json_value_t *parsed = NULL;
    sc_json_parse(&alloc, json100, json100_len, &parsed);
    if (!parsed) {
        sc_str_free(&alloc, json100);
        return;
    }

    BENCH_RUN("json_get_string", JSON_ITERATIONS, {
        (void)sc_json_get_string(parsed, "key_0");
        (void)sc_json_get_string(parsed, "key_3");
        (void)sc_json_get_string(parsed, "key_99");
    });

    BENCH_RUN("json_get_number", JSON_ITERATIONS, {
        (void)sc_json_get_number(parsed, "key_1", 0);
        (void)sc_json_get_number(parsed, "key_4", 0);
        (void)sc_json_get_number(parsed, "key_97", 0);
    });

    sc_json_free(&alloc, parsed);

    /* Build JSON output: sc_json_append_* */
    BENCH_RUN("json_build_object", JSON_ITERATIONS, {
        sc_json_buf_t b;
        sc_json_buf_init(&b, &alloc);
        sc_json_buf_append_raw(&b, "{", 1);
        for (int k = 0; k < 20; k++) {
            if (k > 0)
                sc_json_buf_append_raw(&b, ",", 1);
            sc_json_append_key_value(&b, "name", 4, "bench", 5);
            sc_json_append_key_int(&b, "id", 2, (long long)k);
            sc_json_append_key_bool(&b, "active", 6, true);
        }
        sc_json_buf_append_raw(&b, "}", 1);
        sc_json_buf_free(&b);
    });

    sc_str_free(&alloc, json100);
}
