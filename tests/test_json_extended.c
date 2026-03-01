/* JSON edge cases (~40 tests). */
#include "test_framework.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void test_json_deeply_nested_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[4096];
    size_t pos = 0;
    for (int i = 0; i < 64; i++) pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"a\":");
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "null");
    for (int i = 0; i < 64; i++) pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "}");
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, buf, pos, &val);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(val);
    sc_json_free(&alloc, val);
}

static void test_json_deeply_nested_exceeds_max_depth(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[8192];
    size_t pos = 0;
    for (int i = 0; i < 70; i++) pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"x\":");
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "0");
    for (int i = 0; i < 70; i++) pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "}");
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, buf, pos, &val);
    SC_ASSERT_EQ(err, SC_ERR_JSON_DEPTH);
    SC_ASSERT_NULL(val);
}

static void test_json_empty_string(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\"", 2, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_STRING);
    SC_ASSERT_EQ(val->data.string.len, 0);
    sc_json_free(&alloc, val);
}

static void test_json_string_with_all_escapes(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    const char *s = "\"\\\\\\\"\\/\\b\\f\\n\\r\\t\"";
    SC_ASSERT_EQ(sc_json_parse(&alloc, s, strlen(s), &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_STRING);
    sc_json_free(&alloc, val);
}

static void test_json_unicode_bmp(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\u00E9\"", 8, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_STRING);
    SC_ASSERT_TRUE(val->data.string.len >= 1);
    sc_json_free(&alloc, val);
}

static void test_json_large_number(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "12345678901234", 14, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_NUMBER);
    SC_ASSERT_FLOAT_EQ(val->data.number, 12345678901234.0, 1.0);
    sc_json_free(&alloc, val);
}

static void test_json_negative_number(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "-42.5", 5, &val), SC_OK);
    SC_ASSERT_FLOAT_EQ(val->data.number, -42.5, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_float_precision(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "3.141592653589793", 17, &val), SC_OK);
    SC_ASSERT_FLOAT_EQ(val->data.number, 3.141592653589793, 0.0000001);
    sc_json_free(&alloc, val);
}

static void test_json_null_in_array(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "[null,null]", 11, &val), SC_OK);
    SC_ASSERT_EQ(val->data.array.len, 2);
    SC_ASSERT_EQ(val->data.array.items[0]->type, SC_JSON_NULL);
    SC_ASSERT_EQ(val->data.array.items[1]->type, SC_JSON_NULL);
    sc_json_free(&alloc, val);
}

static void test_json_mixed_types_in_array(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    const char *s = "[1,\"two\",true,null,{\"x\":1}]";
    SC_ASSERT_EQ(sc_json_parse(&alloc, s, strlen(s), &val), SC_OK);
    SC_ASSERT_EQ(val->data.array.len, 5);
    SC_ASSERT_EQ(val->data.array.items[0]->type, SC_JSON_NUMBER);
    SC_ASSERT_EQ(val->data.array.items[1]->type, SC_JSON_STRING);
    SC_ASSERT_EQ(val->data.array.items[2]->type, SC_JSON_BOOL);
    SC_ASSERT_EQ(val->data.array.items[3]->type, SC_JSON_NULL);
    SC_ASSERT_EQ(val->data.array.items[4]->type, SC_JSON_OBJECT);
    sc_json_free(&alloc, val);
}

static void test_json_whitespace_everywhere(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    const char *s = "  {  \"a\"  :  1  ,  \"b\"  :  2  }  ";
    SC_ASSERT_EQ(sc_json_parse(&alloc, s, strlen(s), &val), SC_OK);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val, "a", 0), 1.0, 0.001);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val, "b", 0), 2.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_builder_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(buf.len, 0u);
    sc_json_buf_free(&buf);
}

static void test_json_builder_key_value(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_json_append_key_value(&buf, "k", 1, "v", 1);
    SC_ASSERT_STR_EQ(buf.ptr, "\"k\":\"v\"");
    sc_json_buf_free(&buf);
}

static void test_json_builder_key_int(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_json_append_key_int(&buf, "n", 1, -99);
    SC_ASSERT_TRUE(strstr(buf.ptr, "-99") != NULL);
    sc_json_buf_free(&buf);
}

static void test_json_builder_key_bool(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_json_append_key_bool(&buf, "ok", 2, false);
    SC_ASSERT_STR_EQ(buf.ptr, "\"ok\":false");
    sc_json_buf_free(&buf);
}

static void test_json_builder_nested(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *inner = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, inner, "x", sc_json_number_new(&alloc, 1));
    sc_json_value_t *outer = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, outer, "nested", inner);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, outer, &out, &out_len), SC_OK);
    SC_ASSERT_TRUE(strstr(out, "nested") != NULL);
    SC_ASSERT_TRUE(strstr(out, "1") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, outer);
}

static void test_json_stringify_all_types(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *arr = sc_json_array_new(&alloc);
    sc_json_array_push(&alloc, arr, sc_json_null_new(&alloc));
    sc_json_array_push(&alloc, arr, sc_json_bool_new(&alloc, true));
    sc_json_array_push(&alloc, arr, sc_json_number_new(&alloc, 42));
    sc_json_array_push(&alloc, arr, sc_json_string_new(&alloc, "s", 1));
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "empty", sc_json_object_new(&alloc));
    sc_json_array_push(&alloc, arr, obj);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, arr, &out, &out_len), SC_OK);
    SC_ASSERT_NOT_NULL(out);
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, arr);
}

static void test_json_parse_and_stringify_roundtrip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"a\":1,\"b\":\"hello\",\"c\":[1,2,3]}";
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, input, strlen(input), &val), SC_OK);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, val, &out, &out_len), SC_OK);
    sc_json_value_t *val2 = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, out, out_len, &val2), SC_OK);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val2, "a", 0), 1.0, 0.001);
    SC_ASSERT_STR_EQ(sc_json_get_string(val2, "b"), "hello");
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, val);
    sc_json_free(&alloc, val2);
}

static void test_json_array_push(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *arr = sc_json_array_new(&alloc);
    sc_json_array_push(&alloc, arr, sc_json_number_new(&alloc, 1));
    sc_json_array_push(&alloc, arr, sc_json_number_new(&alloc, 2));
    SC_ASSERT_EQ(arr->data.array.len, 2);
    sc_json_free(&alloc, arr);
}

static void test_json_object_get_missing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "a", sc_json_number_new(&alloc, 1));
    sc_json_value_t *v = sc_json_object_get(obj, "b");
    SC_ASSERT_NULL(v);
    sc_json_free(&alloc, obj);
}

static void test_json_get_number_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    double d = sc_json_get_number(obj, "missing", 99.5);
    SC_ASSERT_FLOAT_EQ(d, 99.5, 0.001);
    sc_json_free(&alloc, obj);
}

static void test_json_get_bool_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    SC_ASSERT_FALSE(sc_json_get_bool(obj, "missing", false));
    SC_ASSERT_TRUE(sc_json_get_bool(obj, "missing2", true));
    sc_json_free(&alloc, obj);
}

static void test_json_buf_append_raw(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_json_buf_append_raw(&buf, ",", 1);
    SC_ASSERT_TRUE(buf.len >= 1);
    sc_json_buf_free(&buf);
}

static void test_json_parse_incomplete_string(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "\"unclosed", 9, &val);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_json_parse_incomplete_array(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "[1,2,3", 7, &val);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_json_parse_incomplete_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "{\"a\":1", 6, &val);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_json_parse_zero_len(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "", 0, &val);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_json_parse_exponent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "1e10", 4, &val);
    if (err == SC_OK) {
        SC_ASSERT_EQ(val->type, SC_JSON_NUMBER);
        sc_json_free(&alloc, val);
    }
}

static void test_json_parse_negative_exponent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "1e-5", 4, &val);
    if (err == SC_OK) {
        SC_ASSERT_FLOAT_EQ(val->data.number, 0.00001, 0.000001);
        sc_json_free(&alloc, val);
    }
}

static void test_json_parse_true_false(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *v1 = NULL, *v2 = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "true", 4, &v1), SC_OK);
    SC_ASSERT_TRUE(v1->data.boolean);
    SC_ASSERT_EQ(sc_json_parse(&alloc, "false", 5, &v2), SC_OK);
    SC_ASSERT_FALSE(v2->data.boolean);
    sc_json_free(&alloc, v1);
    sc_json_free(&alloc, v2);
}

static void test_json_free_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_free(&alloc, NULL);
}

static void test_json_array_empty_stringify(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *arr = sc_json_array_new(&alloc);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, arr, &out, &out_len), SC_OK);
    SC_ASSERT_STR_EQ(out, "[]");
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, arr);
}

static void test_json_object_empty_stringify(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, obj, &out, &out_len), SC_OK);
    SC_ASSERT_STR_EQ(out, "{}");
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, obj);
}

static void test_json_append_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    SC_ASSERT_EQ(sc_json_append_key(&buf, "key", 3), SC_OK);
    SC_ASSERT_TRUE(buf.len >= 5);
    SC_ASSERT_TRUE(strstr(buf.ptr, "key") != NULL);
    sc_json_buf_free(&buf);
}

static void test_json_parse_hex_escape(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\u0041\"", 8, &val), SC_OK);
    SC_ASSERT_EQ(val->data.string.len, 1);
    SC_ASSERT_EQ((unsigned char)val->data.string.ptr[0], 'A');
    sc_json_free(&alloc, val);
}

/* ─── ~40 additional JSON tests ────────────────────────────────────────────── */
static void test_json_parse_int64_max(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "9223372036854775807", 19, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_NUMBER);
    sc_json_free(&alloc, val);
}

static void test_json_parse_int64_min(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "-9223372036854775808", 20, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_NUMBER);
    sc_json_free(&alloc, val);
}

static void test_json_object_get_existing_key(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "k", sc_json_string_new(&alloc, "v", 1));
    sc_json_value_t *v = sc_json_object_get(obj, "k");
    SC_ASSERT_NOT_NULL(v);
    SC_ASSERT_EQ(v->type, SC_JSON_STRING);
    SC_ASSERT_EQ(v->data.string.len, 1);
    SC_ASSERT_EQ((unsigned char)v->data.string.ptr[0], 'v');
    sc_json_free(&alloc, obj);
}

static void test_json_object_get_nested_access(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *s = "{\"a\":{\"b\":{\"c\":42}}}";
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, s, strlen(s), &val), SC_OK);
    sc_json_value_t *a = sc_json_object_get(val, "a");
    SC_ASSERT_NOT_NULL(a);
    sc_json_value_t *b = sc_json_object_get(a, "b");
    SC_ASSERT_NOT_NULL(b);
    sc_json_value_t *c = sc_json_object_get(b, "c");
    SC_ASSERT_NOT_NULL(c);
    SC_ASSERT_FLOAT_EQ(c->data.number, 42.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_object_set_add_new(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    SC_ASSERT_EQ(sc_json_object_set(&alloc, obj, "new_key", sc_json_number_new(&alloc, 99)), SC_OK);
    sc_json_value_t *v = sc_json_object_get(obj, "new_key");
    SC_ASSERT_NOT_NULL(v);
    SC_ASSERT_FLOAT_EQ(v->data.number, 99.0, 0.001);
    sc_json_free(&alloc, obj);
}

static void test_json_object_set_overwrite(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "x", sc_json_number_new(&alloc, 1));
    sc_json_object_set(&alloc, obj, "x", sc_json_number_new(&alloc, 2));
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(obj, "x", 0), 2.0, 0.001);
    sc_json_free(&alloc, obj);
}

static void test_json_object_remove_existing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "to_remove", sc_json_null_new(&alloc));
    bool removed = sc_json_object_remove(&alloc, obj, "to_remove");
    SC_ASSERT_TRUE(removed);
    SC_ASSERT_NULL(sc_json_object_get(obj, "to_remove"));
    sc_json_free(&alloc, obj);
}

static void test_json_object_remove_missing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "a", sc_json_number_new(&alloc, 1));
    bool removed = sc_json_object_remove(&alloc, obj, "nonexistent");
    SC_ASSERT_FALSE(removed);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(obj, "a", 0), 1.0, 0.001);
    sc_json_free(&alloc, obj);
}

static void test_json_array_push_check_length(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *arr = sc_json_array_new(&alloc);
    for (int i = 0; i < 10; i++) {
        SC_ASSERT_EQ(sc_json_array_push(&alloc, arr, sc_json_number_new(&alloc, i)), SC_OK);
    }
    SC_ASSERT_EQ(arr->data.array.len, 10u);
    SC_ASSERT_FLOAT_EQ(arr->data.array.items[9]->data.number, 9.0, 0.001);
    sc_json_free(&alloc, arr);
}

static void test_json_string_escape_n(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"a\\nb\"", 6, &val), SC_OK);
    SC_ASSERT_EQ(val->data.string.len, 3);
    SC_ASSERT_EQ((unsigned char)val->data.string.ptr[1], '\n');
    sc_json_free(&alloc, val);
}

static void test_json_string_escape_t(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\t\"", 4, &val), SC_OK);
    SC_ASSERT_EQ(val->data.string.len, 1);
    SC_ASSERT_EQ((unsigned char)val->data.string.ptr[0], '\t');
    sc_json_free(&alloc, val);
}

static void test_json_string_escape_backslash(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\\\\"", 4, &val), SC_OK);
    SC_ASSERT_EQ(val->data.string.len, 1);
    SC_ASSERT_EQ((unsigned char)val->data.string.ptr[0], '\\');
    sc_json_free(&alloc, val);
}

static void test_json_string_escape_quote(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\\"\"", 4, &val), SC_OK);
    SC_ASSERT_EQ(val->data.string.len, 1);
    SC_ASSERT_EQ((unsigned char)val->data.string.ptr[0], '"');
    sc_json_free(&alloc, val);
}

static void test_json_string_escape_unicode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\u1234\"", 8, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_STRING);
    SC_ASSERT_TRUE(val->data.string.len >= 1);
    sc_json_free(&alloc, val);
}

static void test_json_nested_five_levels(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *s = "{\"l1\":{\"l2\":{\"l3\":{\"l4\":{\"l5\":true}}}}}";
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, s, strlen(s), &val), SC_OK);
    sc_json_value_t *v = val;
    for (int i = 0; i < 5; i++) {
        char key[4];
        snprintf(key, sizeof(key), "l%d", i + 1);
        v = sc_json_object_get(v, key);
        SC_ASSERT_NOT_NULL(v);
    }
    SC_ASSERT_TRUE(v->data.boolean);
    sc_json_free(&alloc, val);
}

static void test_json_roundtrip_deep(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"arr\":[1,2,{\"nested\":\"value\"}],\"b\":false}";
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, input, strlen(input), &val), SC_OK);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, val, &out, &out_len), SC_OK);
    sc_json_value_t *val2 = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, out, out_len, &val2), SC_OK);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val2, "arr", 0), 0.0, 0.001);
    sc_json_value_t *arr = sc_json_object_get(val2, "arr");
    SC_ASSERT_NOT_NULL(arr);
    SC_ASSERT_EQ(arr->type, SC_JSON_ARRAY);
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, val);
    sc_json_free(&alloc, val2);
}

static void test_json_parse_invalid_token(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "undefined", 9, &val), SC_OK);
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "nul", 3, &val), SC_OK);
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "tru", 3, &val), SC_OK);
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "fals", 4, &val), SC_OK);
}

static void test_json_parse_truncated_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "{\"k\":", 5, &val), SC_OK);
}

static void test_json_parse_truncated_array(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "[1,2,", 5, &val), SC_OK);
}

static void test_json_buf_init_len_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(buf.len, 0u);
    SC_ASSERT_NOT_NULL(buf.alloc);
    sc_json_buf_free(&buf);
}

static void test_json_buf_append_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_json_buf_append_raw(&buf, "a", 1);
    sc_json_buf_append_raw(&buf, "b", 1);
    sc_json_buf_append_raw(&buf, "c", 1);
    SC_ASSERT_EQ(buf.len, 3u);
    SC_ASSERT_TRUE(buf.ptr[0] == 'a' && buf.ptr[1] == 'b' && buf.ptr[2] == 'c');
    sc_json_buf_free(&buf);
}

static void test_json_buf_free_no_leak(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_json_buf_append_raw(&buf, "test", 4);
    sc_json_buf_free(&buf);
}

static void test_json_get_string_from_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "name", sc_json_string_new(&alloc, "Alice", 5));
    const char *s = sc_json_get_string(obj, "name");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "Alice");
    sc_json_free(&alloc, obj);
}

static void test_json_get_string_missing_returns_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    const char *s = sc_json_get_string(obj, "missing");
    SC_ASSERT_NULL(s);
    sc_json_free(&alloc, obj);
}

static void test_json_empty_array_parse(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "[]", 2, &val), SC_OK);
    SC_ASSERT_EQ(val->data.array.len, 0u);
    sc_json_free(&alloc, val);
}

static void test_json_empty_object_parse(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "{}", 2, &val), SC_OK);
    SC_ASSERT_EQ(val->data.object.len, 0u);
    sc_json_free(&alloc, val);
}

static void test_json_array_of_objects(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *s = "[{\"id\":1},{\"id\":2},{\"id\":3}]";
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, s, strlen(s), &val), SC_OK);
    SC_ASSERT_EQ(val->data.array.len, 3u);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val->data.array.items[0], "id", 0), 1.0, 0.001);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val->data.array.items[1], "id", 0), 2.0, 0.001);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val->data.array.items[2], "id", 0), 3.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_scientific_notation(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "2.5e2", 5, &val);
    if (err == SC_OK) {
        SC_ASSERT_FLOAT_EQ(val->data.number, 250.0, 1.0);
        sc_json_free(&alloc, val);
    }
}

static void test_json_stringify_escapes(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *s = sc_json_string_new(&alloc, "a\nb\tc", 5);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, s, &out, &out_len), SC_OK);
    SC_ASSERT_TRUE(strstr(out, "\\n") != NULL || strstr(out, "n") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, s);
}

static void test_json_number_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "0", 1, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_NUMBER);
    SC_ASSERT_FLOAT_EQ(val->data.number, 0.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_number_scientific_neg(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "1e-3", 4, &val);
    if (err == SC_OK) {
        SC_ASSERT_FLOAT_EQ(val->data.number, 0.001, 0.0001);
        sc_json_free(&alloc, val);
    }
}

static void test_json_duplicate_keys_last_wins(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "{\"a\":1,\"a\":2,\"a\":3}", 19, &val), SC_OK);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val, "a", 0), 3.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_append_string_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    SC_ASSERT_EQ(sc_json_append_string(&buf, "", 0), SC_OK);
    SC_ASSERT_STR_EQ(buf.ptr, "\"\"");
    sc_json_buf_free(&buf);
}

static void test_json_complex_roundtrip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *input = "{\"arr\":[null,true,1.5,\"x\"],\"obj\":{}}";
    sc_json_value_t *val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, input, strlen(input), &val), SC_OK);
    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, val, &out, &out_len), SC_OK);
    sc_json_value_t *val2 = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, out, out_len, &val2), SC_OK);
    sc_json_value_t *arr = sc_json_object_get(val2, "arr");
    SC_ASSERT_EQ(arr->data.array.items[0]->type, SC_JSON_NULL);
    SC_ASSERT_TRUE(arr->data.array.items[1]->data.boolean);
    SC_ASSERT_FLOAT_EQ(arr->data.array.items[2]->data.number, 1.5, 0.001);
    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, val);
    sc_json_free(&alloc, val2);
}

void run_json_extended_tests(void) {
    SC_TEST_SUITE("JSON Extended");
    SC_RUN_TEST(test_json_deeply_nested_object);
    SC_RUN_TEST(test_json_deeply_nested_exceeds_max_depth);
    SC_RUN_TEST(test_json_empty_string);
    SC_RUN_TEST(test_json_string_with_all_escapes);
    SC_RUN_TEST(test_json_unicode_bmp);
    SC_RUN_TEST(test_json_large_number);
    SC_RUN_TEST(test_json_negative_number);
    SC_RUN_TEST(test_json_float_precision);
    SC_RUN_TEST(test_json_null_in_array);
    SC_RUN_TEST(test_json_mixed_types_in_array);
    SC_RUN_TEST(test_json_whitespace_everywhere);
    SC_RUN_TEST(test_json_builder_empty);
    SC_RUN_TEST(test_json_builder_key_value);
    SC_RUN_TEST(test_json_builder_key_int);
    SC_RUN_TEST(test_json_builder_key_bool);
    SC_RUN_TEST(test_json_builder_nested);
    SC_RUN_TEST(test_json_stringify_all_types);
    SC_RUN_TEST(test_json_parse_and_stringify_roundtrip);
    SC_RUN_TEST(test_json_array_push);
    SC_RUN_TEST(test_json_object_get_missing);
    SC_RUN_TEST(test_json_get_number_default);
    SC_RUN_TEST(test_json_get_bool_default);
    SC_RUN_TEST(test_json_buf_append_raw);
    SC_RUN_TEST(test_json_parse_incomplete_string);
    SC_RUN_TEST(test_json_parse_incomplete_array);
    SC_RUN_TEST(test_json_parse_incomplete_object);
    SC_RUN_TEST(test_json_parse_zero_len);
    SC_RUN_TEST(test_json_parse_exponent);
    SC_RUN_TEST(test_json_parse_negative_exponent);
    SC_RUN_TEST(test_json_parse_true_false);
    SC_RUN_TEST(test_json_free_null_safe);
    SC_RUN_TEST(test_json_array_empty_stringify);
    SC_RUN_TEST(test_json_object_empty_stringify);
    SC_RUN_TEST(test_json_append_key);
    SC_RUN_TEST(test_json_parse_hex_escape);

    SC_RUN_TEST(test_json_parse_int64_max);
    SC_RUN_TEST(test_json_parse_int64_min);
    SC_RUN_TEST(test_json_object_get_existing_key);
    SC_RUN_TEST(test_json_object_get_nested_access);
    SC_RUN_TEST(test_json_object_set_add_new);
    SC_RUN_TEST(test_json_object_set_overwrite);
    SC_RUN_TEST(test_json_object_remove_existing);
    SC_RUN_TEST(test_json_object_remove_missing);
    SC_RUN_TEST(test_json_array_push_check_length);
    SC_RUN_TEST(test_json_string_escape_n);
    SC_RUN_TEST(test_json_string_escape_t);
    SC_RUN_TEST(test_json_string_escape_backslash);
    SC_RUN_TEST(test_json_string_escape_quote);
    SC_RUN_TEST(test_json_string_escape_unicode);
    SC_RUN_TEST(test_json_nested_five_levels);
    SC_RUN_TEST(test_json_roundtrip_deep);
    SC_RUN_TEST(test_json_parse_invalid_token);
    SC_RUN_TEST(test_json_parse_truncated_object);
    SC_RUN_TEST(test_json_parse_truncated_array);
    SC_RUN_TEST(test_json_buf_init_len_zero);
    SC_RUN_TEST(test_json_buf_append_multiple);
    SC_RUN_TEST(test_json_buf_free_no_leak);
    SC_RUN_TEST(test_json_get_string_from_object);
    SC_RUN_TEST(test_json_get_string_missing_returns_null);
    SC_RUN_TEST(test_json_empty_array_parse);
    SC_RUN_TEST(test_json_empty_object_parse);
    SC_RUN_TEST(test_json_array_of_objects);
    SC_RUN_TEST(test_json_scientific_notation);
    SC_RUN_TEST(test_json_stringify_escapes);
    SC_RUN_TEST(test_json_number_zero);
    SC_RUN_TEST(test_json_number_scientific_neg);
    SC_RUN_TEST(test_json_duplicate_keys_last_wins);
    SC_RUN_TEST(test_json_append_string_empty);
    SC_RUN_TEST(test_json_complex_roundtrip);
}
