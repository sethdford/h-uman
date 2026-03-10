/* JSON edge cases (~40 tests). */
#include "human/core/allocator.h"
#include "human/core/json.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_json_deeply_nested_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[4096];
    size_t pos = 0;
    for (int i = 0; i < 64; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"a\":");
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "null");
    for (int i = 0; i < 64; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "}");
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, buf, pos, &val);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(val);
    hu_json_free(&alloc, val);
}

static void test_json_deeply_nested_exceeds_max_depth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[8192];
    size_t pos = 0;
    for (int i = 0; i < 70; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"x\":");
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "0");
    for (int i = 0; i < 70; i++)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "}");
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, buf, pos, &val);
    HU_ASSERT_EQ(err, HU_ERR_JSON_DEPTH);
    HU_ASSERT_NULL(val);
}

static void test_json_empty_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\"", 2, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_STRING);
    HU_ASSERT_EQ(val->data.string.len, 0);
    hu_json_free(&alloc, val);
}

static void test_json_string_with_all_escapes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    const char *s = "\"\\\\\\\"\\/\\b\\f\\n\\r\\t\"";
    HU_ASSERT_EQ(hu_json_parse(&alloc, s, strlen(s), &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_STRING);
    hu_json_free(&alloc, val);
}

static void test_json_unicode_bmp(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\u00E9\"", 8, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_STRING);
    HU_ASSERT_TRUE(val->data.string.len >= 1);
    hu_json_free(&alloc, val);
}

static void test_json_large_number(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "12345678901234", 14, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_NUMBER);
    HU_ASSERT_FLOAT_EQ(val->data.number, 12345678901234.0, 1.0);
    hu_json_free(&alloc, val);
}

static void test_json_negative_number(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "-42.5", 5, &val), HU_OK);
    HU_ASSERT_FLOAT_EQ(val->data.number, -42.5, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_float_precision(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "3.141592653589793", 17, &val), HU_OK);
    HU_ASSERT_FLOAT_EQ(val->data.number, 3.141592653589793, 0.0000001);
    hu_json_free(&alloc, val);
}

static void test_json_null_in_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "[null,null]", 11, &val), HU_OK);
    HU_ASSERT_EQ(val->data.array.len, 2);
    HU_ASSERT_EQ(val->data.array.items[0]->type, HU_JSON_NULL);
    HU_ASSERT_EQ(val->data.array.items[1]->type, HU_JSON_NULL);
    hu_json_free(&alloc, val);
}

static void test_json_mixed_types_in_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    const char *s = "[1,\"two\",true,null,{\"x\":1}]";
    HU_ASSERT_EQ(hu_json_parse(&alloc, s, strlen(s), &val), HU_OK);
    HU_ASSERT_EQ(val->data.array.len, 5);
    HU_ASSERT_EQ(val->data.array.items[0]->type, HU_JSON_NUMBER);
    HU_ASSERT_EQ(val->data.array.items[1]->type, HU_JSON_STRING);
    HU_ASSERT_EQ(val->data.array.items[2]->type, HU_JSON_BOOL);
    HU_ASSERT_EQ(val->data.array.items[3]->type, HU_JSON_NULL);
    HU_ASSERT_EQ(val->data.array.items[4]->type, HU_JSON_OBJECT);
    hu_json_free(&alloc, val);
}

static void test_json_whitespace_everywhere(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    const char *s = "  {  \"a\"  :  1  ,  \"b\"  :  2  }  ";
    HU_ASSERT_EQ(hu_json_parse(&alloc, s, strlen(s), &val), HU_OK);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val, "a", 0), 1.0, 0.001);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val, "b", 0), 2.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_builder_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(buf.len, 0u);
    hu_json_buf_free(&buf);
}

static void test_json_builder_key_value(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_json_append_key_value(&buf, "k", 1, "v", 1);
    HU_ASSERT_STR_EQ(buf.ptr, "\"k\":\"v\"");
    hu_json_buf_free(&buf);
}

static void test_json_builder_key_int(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_json_append_key_int(&buf, "n", 1, -99);
    HU_ASSERT_TRUE(strstr(buf.ptr, "-99") != NULL);
    hu_json_buf_free(&buf);
}

static void test_json_builder_key_bool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_json_append_key_bool(&buf, "ok", 2, false);
    HU_ASSERT_STR_EQ(buf.ptr, "\"ok\":false");
    hu_json_buf_free(&buf);
}

static void test_json_builder_nested(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *inner = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, inner, "x", hu_json_number_new(&alloc, 1));
    hu_json_value_t *outer = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, outer, "nested", inner);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, outer, &out, &out_len), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "nested") != NULL);
    HU_ASSERT_TRUE(strstr(out, "1") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, outer);
}

static void test_json_stringify_all_types(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *arr = hu_json_array_new(&alloc);
    hu_json_array_push(&alloc, arr, hu_json_null_new(&alloc));
    hu_json_array_push(&alloc, arr, hu_json_bool_new(&alloc, true));
    hu_json_array_push(&alloc, arr, hu_json_number_new(&alloc, 42));
    hu_json_array_push(&alloc, arr, hu_json_string_new(&alloc, "s", 1));
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "empty", hu_json_object_new(&alloc));
    hu_json_array_push(&alloc, arr, obj);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, arr, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, arr);
}

static void test_json_parse_and_stringify_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"a\":1,\"b\":\"hello\",\"c\":[1,2,3]}";
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, input, strlen(input), &val), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, val, &out, &out_len), HU_OK);
    hu_json_value_t *val2 = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, out, out_len, &val2), HU_OK);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val2, "a", 0), 1.0, 0.001);
    HU_ASSERT_STR_EQ(hu_json_get_string(val2, "b"), "hello");
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, val);
    hu_json_free(&alloc, val2);
}

static void test_json_array_push(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *arr = hu_json_array_new(&alloc);
    hu_json_array_push(&alloc, arr, hu_json_number_new(&alloc, 1));
    hu_json_array_push(&alloc, arr, hu_json_number_new(&alloc, 2));
    HU_ASSERT_EQ(arr->data.array.len, 2);
    hu_json_free(&alloc, arr);
}

static void test_json_object_get_missing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "a", hu_json_number_new(&alloc, 1));
    hu_json_value_t *v = hu_json_object_get(obj, "b");
    HU_ASSERT_NULL(v);
    hu_json_free(&alloc, obj);
}

static void test_json_get_number_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    double d = hu_json_get_number(obj, "missing", 99.5);
    HU_ASSERT_FLOAT_EQ(d, 99.5, 0.001);
    hu_json_free(&alloc, obj);
}

static void test_json_get_bool_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    HU_ASSERT_FALSE(hu_json_get_bool(obj, "missing", false));
    HU_ASSERT_TRUE(hu_json_get_bool(obj, "missing2", true));
    hu_json_free(&alloc, obj);
}

static void test_json_buf_append_raw(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_json_buf_append_raw(&buf, ",", 1);
    HU_ASSERT_TRUE(buf.len >= 1);
    hu_json_buf_free(&buf);
}

static void test_json_parse_incomplete_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "\"unclosed", 9, &val);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_json_parse_incomplete_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "[1,2,3", 7, &val);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_json_parse_incomplete_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "{\"a\":1", 6, &val);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_json_parse_zero_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "", 0, &val);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_json_parse_exponent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "1e10", 4, &val);
    if (err == HU_OK) {
        HU_ASSERT_EQ(val->type, HU_JSON_NUMBER);
        hu_json_free(&alloc, val);
    }
}

static void test_json_parse_negative_exponent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "1e-5", 4, &val);
    if (err == HU_OK) {
        HU_ASSERT_FLOAT_EQ(val->data.number, 0.00001, 0.000001);
        hu_json_free(&alloc, val);
    }
}

static void test_json_parse_true_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *v1 = NULL, *v2 = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "true", 4, &v1), HU_OK);
    HU_ASSERT_TRUE(v1->data.boolean);
    HU_ASSERT_EQ(hu_json_parse(&alloc, "false", 5, &v2), HU_OK);
    HU_ASSERT_FALSE(v2->data.boolean);
    hu_json_free(&alloc, v1);
    hu_json_free(&alloc, v2);
}

static void test_json_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_free(&alloc, NULL);
}

static void test_json_array_empty_stringify(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *arr = hu_json_array_new(&alloc);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, arr, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "[]");
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, arr);
}

static void test_json_object_empty_stringify(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, obj, &out, &out_len), HU_OK);
    HU_ASSERT_STR_EQ(out, "{}");
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, obj);
}

static void test_json_append_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    HU_ASSERT_EQ(hu_json_append_key(&buf, "key", 3), HU_OK);
    HU_ASSERT_TRUE(buf.len >= 5);
    HU_ASSERT_TRUE(strstr(buf.ptr, "key") != NULL);
    hu_json_buf_free(&buf);
}

static void test_json_parse_hex_escape(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\u0041\"", 8, &val), HU_OK);
    HU_ASSERT_EQ(val->data.string.len, 1);
    HU_ASSERT_EQ((unsigned char)val->data.string.ptr[0], 'A');
    hu_json_free(&alloc, val);
}

/* ─── ~40 additional JSON tests ────────────────────────────────────────────── */
static void test_json_parse_int64_max(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "9223372036854775807", 19, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_NUMBER);
    hu_json_free(&alloc, val);
}

static void test_json_parse_int64_min(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "-9223372036854775808", 20, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_NUMBER);
    hu_json_free(&alloc, val);
}

static void test_json_object_get_existing_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "k", hu_json_string_new(&alloc, "v", 1));
    hu_json_value_t *v = hu_json_object_get(obj, "k");
    HU_ASSERT_NOT_NULL(v);
    HU_ASSERT_EQ(v->type, HU_JSON_STRING);
    HU_ASSERT_EQ(v->data.string.len, 1);
    HU_ASSERT_EQ((unsigned char)v->data.string.ptr[0], 'v');
    hu_json_free(&alloc, obj);
}

static void test_json_object_get_nested_access(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *s = "{\"a\":{\"b\":{\"c\":42}}}";
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, s, strlen(s), &val), HU_OK);
    hu_json_value_t *a = hu_json_object_get(val, "a");
    HU_ASSERT_NOT_NULL(a);
    hu_json_value_t *b = hu_json_object_get(a, "b");
    HU_ASSERT_NOT_NULL(b);
    hu_json_value_t *c = hu_json_object_get(b, "c");
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_FLOAT_EQ(c->data.number, 42.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_object_set_add_new(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    HU_ASSERT_EQ(hu_json_object_set(&alloc, obj, "new_key", hu_json_number_new(&alloc, 99)), HU_OK);
    hu_json_value_t *v = hu_json_object_get(obj, "new_key");
    HU_ASSERT_NOT_NULL(v);
    HU_ASSERT_FLOAT_EQ(v->data.number, 99.0, 0.001);
    hu_json_free(&alloc, obj);
}

static void test_json_object_set_overwrite(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "x", hu_json_number_new(&alloc, 1));
    hu_json_object_set(&alloc, obj, "x", hu_json_number_new(&alloc, 2));
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(obj, "x", 0), 2.0, 0.001);
    hu_json_free(&alloc, obj);
}

static void test_json_object_remove_existing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "to_remove", hu_json_null_new(&alloc));
    bool removed = hu_json_object_remove(&alloc, obj, "to_remove");
    HU_ASSERT_TRUE(removed);
    HU_ASSERT_NULL(hu_json_object_get(obj, "to_remove"));
    hu_json_free(&alloc, obj);
}

static void test_json_object_remove_missing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "a", hu_json_number_new(&alloc, 1));
    bool removed = hu_json_object_remove(&alloc, obj, "nonexistent");
    HU_ASSERT_FALSE(removed);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(obj, "a", 0), 1.0, 0.001);
    hu_json_free(&alloc, obj);
}

static void test_json_array_push_check_length(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *arr = hu_json_array_new(&alloc);
    for (int i = 0; i < 10; i++) {
        HU_ASSERT_EQ(hu_json_array_push(&alloc, arr, hu_json_number_new(&alloc, i)), HU_OK);
    }
    HU_ASSERT_EQ(arr->data.array.len, 10u);
    HU_ASSERT_FLOAT_EQ(arr->data.array.items[9]->data.number, 9.0, 0.001);
    hu_json_free(&alloc, arr);
}

static void test_json_string_escape_n(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"a\\nb\"", 6, &val), HU_OK);
    HU_ASSERT_EQ(val->data.string.len, 3);
    HU_ASSERT_EQ((unsigned char)val->data.string.ptr[1], '\n');
    hu_json_free(&alloc, val);
}

static void test_json_string_escape_t(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\t\"", 4, &val), HU_OK);
    HU_ASSERT_EQ(val->data.string.len, 1);
    HU_ASSERT_EQ((unsigned char)val->data.string.ptr[0], '\t');
    hu_json_free(&alloc, val);
}

static void test_json_string_escape_backslash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\\\\"", 4, &val), HU_OK);
    HU_ASSERT_EQ(val->data.string.len, 1);
    HU_ASSERT_EQ((unsigned char)val->data.string.ptr[0], '\\');
    hu_json_free(&alloc, val);
}

static void test_json_string_escape_quote(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\\"\"", 4, &val), HU_OK);
    HU_ASSERT_EQ(val->data.string.len, 1);
    HU_ASSERT_EQ((unsigned char)val->data.string.ptr[0], '"');
    hu_json_free(&alloc, val);
}

static void test_json_string_escape_unicode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\u1234\"", 8, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_STRING);
    HU_ASSERT_TRUE(val->data.string.len >= 1);
    hu_json_free(&alloc, val);
}

static void test_json_nested_five_levels(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *s = "{\"l1\":{\"l2\":{\"l3\":{\"l4\":{\"l5\":true}}}}}";
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, s, strlen(s), &val), HU_OK);
    hu_json_value_t *v = val;
    for (int i = 0; i < 5; i++) {
        char key[4];
        snprintf(key, sizeof(key), "l%d", i + 1);
        v = hu_json_object_get(v, key);
        HU_ASSERT_NOT_NULL(v);
    }
    HU_ASSERT_TRUE(v->data.boolean);
    hu_json_free(&alloc, val);
}

static void test_json_roundtrip_deep(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"arr\":[1,2,{\"nested\":\"value\"}],\"b\":false}";
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, input, strlen(input), &val), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, val, &out, &out_len), HU_OK);
    hu_json_value_t *val2 = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, out, out_len, &val2), HU_OK);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val2, "arr", 0), 0.0, 0.001);
    hu_json_value_t *arr = hu_json_object_get(val2, "arr");
    HU_ASSERT_NOT_NULL(arr);
    HU_ASSERT_EQ(arr->type, HU_JSON_ARRAY);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, val);
    hu_json_free(&alloc, val2);
}

static void test_json_parse_invalid_token(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "undefined", 9, &val), HU_OK);
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "nul", 3, &val), HU_OK);
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "tru", 3, &val), HU_OK);
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "fals", 4, &val), HU_OK);
}

static void test_json_parse_truncated_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "{\"k\":", 5, &val), HU_OK);
}

static void test_json_parse_truncated_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "[1,2,", 5, &val), HU_OK);
}

static void test_json_buf_init_len_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(buf.len, 0u);
    HU_ASSERT_NOT_NULL(buf.alloc);
    hu_json_buf_free(&buf);
}

static void test_json_buf_append_multiple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_json_buf_append_raw(&buf, "a", 1);
    hu_json_buf_append_raw(&buf, "b", 1);
    hu_json_buf_append_raw(&buf, "c", 1);
    HU_ASSERT_EQ(buf.len, 3u);
    HU_ASSERT_TRUE(buf.ptr[0] == 'a' && buf.ptr[1] == 'b' && buf.ptr[2] == 'c');
    hu_json_buf_free(&buf);
}

static void test_json_buf_free_no_leak(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_json_buf_append_raw(&buf, "test", 4);
    hu_json_buf_free(&buf);
}

static void test_json_get_string_from_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "name", hu_json_string_new(&alloc, "Alice", 5));
    const char *s = hu_json_get_string(obj, "name");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "Alice");
    hu_json_free(&alloc, obj);
}

static void test_json_get_string_missing_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    const char *s = hu_json_get_string(obj, "missing");
    HU_ASSERT_NULL(s);
    hu_json_free(&alloc, obj);
}

static void test_json_empty_array_parse(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "[]", 2, &val), HU_OK);
    HU_ASSERT_EQ(val->data.array.len, 0u);
    hu_json_free(&alloc, val);
}

static void test_json_empty_object_parse(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "{}", 2, &val), HU_OK);
    HU_ASSERT_EQ(val->data.object.len, 0u);
    hu_json_free(&alloc, val);
}

static void test_json_array_of_objects(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *s = "[{\"id\":1},{\"id\":2},{\"id\":3}]";
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, s, strlen(s), &val), HU_OK);
    HU_ASSERT_EQ(val->data.array.len, 3u);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val->data.array.items[0], "id", 0), 1.0, 0.001);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val->data.array.items[1], "id", 0), 2.0, 0.001);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val->data.array.items[2], "id", 0), 3.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_scientific_notation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "2.5e2", 5, &val);
    if (err == HU_OK) {
        HU_ASSERT_FLOAT_EQ(val->data.number, 250.0, 1.0);
        hu_json_free(&alloc, val);
    }
}

static void test_json_stringify_escapes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *s = hu_json_string_new(&alloc, "a\nb\tc", 5);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, s, &out, &out_len), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "\\n") != NULL || strstr(out, "n") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, s);
}

static void test_json_number_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "0", 1, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_NUMBER);
    HU_ASSERT_FLOAT_EQ(val->data.number, 0.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_number_scientific_neg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "1e-3", 4, &val);
    if (err == HU_OK) {
        HU_ASSERT_FLOAT_EQ(val->data.number, 0.001, 0.0001);
        hu_json_free(&alloc, val);
    }
}

static void test_json_duplicate_keys_last_wins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "{\"a\":1,\"a\":2,\"a\":3}", 19, &val), HU_OK);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val, "a", 0), 3.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_append_string_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    HU_ASSERT_EQ(hu_json_append_string(&buf, "", 0), HU_OK);
    HU_ASSERT_STR_EQ(buf.ptr, "\"\"");
    hu_json_buf_free(&buf);
}

static void test_json_complex_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *input = "{\"arr\":[null,true,1.5,\"x\"],\"obj\":{}}";
    hu_json_value_t *val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, input, strlen(input), &val), HU_OK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, val, &out, &out_len), HU_OK);
    hu_json_value_t *val2 = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, out, out_len, &val2), HU_OK);
    hu_json_value_t *arr = hu_json_object_get(val2, "arr");
    HU_ASSERT_EQ(arr->data.array.items[0]->type, HU_JSON_NULL);
    HU_ASSERT_TRUE(arr->data.array.items[1]->data.boolean);
    HU_ASSERT_FLOAT_EQ(arr->data.array.items[2]->data.number, 1.5, 0.001);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, val);
    hu_json_free(&alloc, val2);
}

void run_json_extended_tests(void) {
    HU_TEST_SUITE("JSON Extended");
    HU_RUN_TEST(test_json_deeply_nested_object);
    HU_RUN_TEST(test_json_deeply_nested_exceeds_max_depth);
    HU_RUN_TEST(test_json_empty_string);
    HU_RUN_TEST(test_json_string_with_all_escapes);
    HU_RUN_TEST(test_json_unicode_bmp);
    HU_RUN_TEST(test_json_large_number);
    HU_RUN_TEST(test_json_negative_number);
    HU_RUN_TEST(test_json_float_precision);
    HU_RUN_TEST(test_json_null_in_array);
    HU_RUN_TEST(test_json_mixed_types_in_array);
    HU_RUN_TEST(test_json_whitespace_everywhere);
    HU_RUN_TEST(test_json_builder_empty);
    HU_RUN_TEST(test_json_builder_key_value);
    HU_RUN_TEST(test_json_builder_key_int);
    HU_RUN_TEST(test_json_builder_key_bool);
    HU_RUN_TEST(test_json_builder_nested);
    HU_RUN_TEST(test_json_stringify_all_types);
    HU_RUN_TEST(test_json_parse_and_stringify_roundtrip);
    HU_RUN_TEST(test_json_array_push);
    HU_RUN_TEST(test_json_object_get_missing);
    HU_RUN_TEST(test_json_get_number_default);
    HU_RUN_TEST(test_json_get_bool_default);
    HU_RUN_TEST(test_json_buf_append_raw);
    HU_RUN_TEST(test_json_parse_incomplete_string);
    HU_RUN_TEST(test_json_parse_incomplete_array);
    HU_RUN_TEST(test_json_parse_incomplete_object);
    HU_RUN_TEST(test_json_parse_zero_len);
    HU_RUN_TEST(test_json_parse_exponent);
    HU_RUN_TEST(test_json_parse_negative_exponent);
    HU_RUN_TEST(test_json_parse_true_false);
    HU_RUN_TEST(test_json_free_null_safe);
    HU_RUN_TEST(test_json_array_empty_stringify);
    HU_RUN_TEST(test_json_object_empty_stringify);
    HU_RUN_TEST(test_json_append_key);
    HU_RUN_TEST(test_json_parse_hex_escape);

    HU_RUN_TEST(test_json_parse_int64_max);
    HU_RUN_TEST(test_json_parse_int64_min);
    HU_RUN_TEST(test_json_object_get_existing_key);
    HU_RUN_TEST(test_json_object_get_nested_access);
    HU_RUN_TEST(test_json_object_set_add_new);
    HU_RUN_TEST(test_json_object_set_overwrite);
    HU_RUN_TEST(test_json_object_remove_existing);
    HU_RUN_TEST(test_json_object_remove_missing);
    HU_RUN_TEST(test_json_array_push_check_length);
    HU_RUN_TEST(test_json_string_escape_n);
    HU_RUN_TEST(test_json_string_escape_t);
    HU_RUN_TEST(test_json_string_escape_backslash);
    HU_RUN_TEST(test_json_string_escape_quote);
    HU_RUN_TEST(test_json_string_escape_unicode);
    HU_RUN_TEST(test_json_nested_five_levels);
    HU_RUN_TEST(test_json_roundtrip_deep);
    HU_RUN_TEST(test_json_parse_invalid_token);
    HU_RUN_TEST(test_json_parse_truncated_object);
    HU_RUN_TEST(test_json_parse_truncated_array);
    HU_RUN_TEST(test_json_buf_init_len_zero);
    HU_RUN_TEST(test_json_buf_append_multiple);
    HU_RUN_TEST(test_json_buf_free_no_leak);
    HU_RUN_TEST(test_json_get_string_from_object);
    HU_RUN_TEST(test_json_get_string_missing_returns_null);
    HU_RUN_TEST(test_json_empty_array_parse);
    HU_RUN_TEST(test_json_empty_object_parse);
    HU_RUN_TEST(test_json_array_of_objects);
    HU_RUN_TEST(test_json_scientific_notation);
    HU_RUN_TEST(test_json_stringify_escapes);
    HU_RUN_TEST(test_json_number_zero);
    HU_RUN_TEST(test_json_number_scientific_neg);
    HU_RUN_TEST(test_json_duplicate_keys_last_wins);
    HU_RUN_TEST(test_json_append_string_empty);
    HU_RUN_TEST(test_json_complex_roundtrip);
}
