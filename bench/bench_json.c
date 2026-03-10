/* JSON API benchmarks: parse, access, build. */
#include "bench.h"
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_ITERATIONS 10000

static char *make_medium_json(hu_allocator_t *alloc, size_t num_keys) {
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, alloc);
    hu_json_buf_append_raw(&buf, "{", 1);
    for (size_t i = 0; i < num_keys; i++) {
        if (i > 0)
            hu_json_buf_append_raw(&buf, ",", 1);
        char key[32];
        int n = snprintf(key, sizeof(key), "\"key_%zu\"", i);
        hu_json_buf_append_raw(&buf, key, (size_t)n);
        hu_json_buf_append_raw(&buf, ":", 1);
        if (i % 3 == 0) {
            char val[32];
            n = snprintf(val, sizeof(val), "\"value_%zu\"", i);
            hu_json_buf_append_raw(&buf, val, (size_t)n);
        } else if (i % 3 == 1) {
            char val[16];
            n = snprintf(val, sizeof(val), "%zu", i);
            hu_json_buf_append_raw(&buf, val, (size_t)n);
        } else {
            hu_json_buf_append_raw(&buf, "true", 4);
        }
    }
    hu_json_buf_append_raw(&buf, "}", 1);
    char *out = buf.ptr ? strdup(buf.ptr) : NULL;
    hu_json_buf_free(&buf);
    return out;
}

void run_bench_json(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Parse: medium object with 100 keys */
    char *json100 = make_medium_json(&alloc, 100);
    if (!json100) {
        fprintf(stderr, "bench_json: failed to build medium JSON\n");
        return;
    }
    size_t json100_len = strlen(json100);

    BENCH_RUN("json_parse_100keys", JSON_ITERATIONS, {
        hu_json_value_t *val = NULL;
        hu_json_parse(&alloc, json100, json100_len, &val);
        if (val)
            hu_json_free(&alloc, val);
    });

    /* Access nested fields: parse once, then access */
    hu_json_value_t *parsed = NULL;
    hu_json_parse(&alloc, json100, json100_len, &parsed);
    if (!parsed) {
        hu_str_free(&alloc, json100);
        return;
    }

    BENCH_RUN("json_get_string", JSON_ITERATIONS, {
        (void)hu_json_get_string(parsed, "key_0");
        (void)hu_json_get_string(parsed, "key_3");
        (void)hu_json_get_string(parsed, "key_99");
    });

    BENCH_RUN("json_get_number", JSON_ITERATIONS, {
        (void)hu_json_get_number(parsed, "key_1", 0);
        (void)hu_json_get_number(parsed, "key_4", 0);
        (void)hu_json_get_number(parsed, "key_97", 0);
    });

    hu_json_free(&alloc, parsed);

    /* Build JSON output: hu_json_append_* */
    BENCH_RUN("json_build_object", JSON_ITERATIONS, {
        hu_json_buf_t b;
        hu_json_buf_init(&b, &alloc);
        hu_json_buf_append_raw(&b, "{", 1);
        for (int k = 0; k < 20; k++) {
            if (k > 0)
                hu_json_buf_append_raw(&b, ",", 1);
            hu_json_append_key_value(&b, "name", 4, "bench", 5);
            hu_json_append_key_int(&b, "id", 2, (long long)k);
            hu_json_append_key_bool(&b, "active", 6, true);
        }
        hu_json_buf_append_raw(&b, "}", 1);
        hu_json_buf_free(&b);
    });

    hu_str_free(&alloc, json100);
}
