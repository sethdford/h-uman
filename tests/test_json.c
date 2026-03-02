#include "test_framework.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static void test_json_parse_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    sc_error_t err = sc_json_parse(&alloc, "null", 4, &val);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(val);
    SC_ASSERT_EQ(val->type, SC_JSON_NULL);
    sc_json_free(&alloc, val);
}

static void test_json_parse_bool(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "true", 4, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_BOOL);
    SC_ASSERT_TRUE(val->data.boolean);
    sc_json_free(&alloc, val);

    SC_ASSERT_EQ(sc_json_parse(&alloc, "false", 5, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_BOOL);
    SC_ASSERT_FALSE(val->data.boolean);
    sc_json_free(&alloc, val);
}

static void test_json_parse_number(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "42", 2, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_NUMBER);
    SC_ASSERT_FLOAT_EQ(val->data.number, 42.0, 0.001);
    sc_json_free(&alloc, val);

    SC_ASSERT_EQ(sc_json_parse(&alloc, "-3.14", 5, &val), SC_OK);
    SC_ASSERT_FLOAT_EQ(val->data.number, -3.14, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_parse_string(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"hello\"", 7, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_STRING);
    SC_ASSERT_STR_EQ(val->data.string.ptr, "hello");
    SC_ASSERT_EQ(val->data.string.len, 5);
    sc_json_free(&alloc, val);
}

static void test_json_parse_string_escapes(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"a\\nb\\tc\"", 9, &val), SC_OK);
    SC_ASSERT_STR_EQ(val->data.string.ptr, "a\nb\tc");
    sc_json_free(&alloc, val);
}

static void test_json_parse_array(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    const char *input = "[1, \"two\", true, null]";

    SC_ASSERT_EQ(sc_json_parse(&alloc, input, strlen(input), &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_ARRAY);
    SC_ASSERT_EQ(val->data.array.len, 4);
    SC_ASSERT_EQ(val->data.array.items[0]->type, SC_JSON_NUMBER);
    SC_ASSERT_EQ(val->data.array.items[1]->type, SC_JSON_STRING);
    SC_ASSERT_EQ(val->data.array.items[2]->type, SC_JSON_BOOL);
    SC_ASSERT_EQ(val->data.array.items[3]->type, SC_JSON_NULL);
    sc_json_free(&alloc, val);
}

static void test_json_parse_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    const char *input = "{\"name\": \"seaclaw\", \"version\": 1, \"active\": true}";

    SC_ASSERT_EQ(sc_json_parse(&alloc, input, strlen(input), &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_OBJECT);
    SC_ASSERT_STR_EQ(sc_json_get_string(val, "name"), "seaclaw");
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val, "version", 0), 1.0, 0.001);
    SC_ASSERT_TRUE(sc_json_get_bool(val, "active", false));
    sc_json_free(&alloc, val);
}

static void test_json_parse_nested(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;
    const char *input = "{\"data\": {\"items\": [1, 2, 3]}}";

    SC_ASSERT_EQ(sc_json_parse(&alloc, input, strlen(input), &val), SC_OK);
    sc_json_value_t *data = sc_json_object_get(val, "data");
    SC_ASSERT_NOT_NULL(data);
    sc_json_value_t *items = sc_json_object_get(data, "items");
    SC_ASSERT_NOT_NULL(items);
    SC_ASSERT_EQ(items->type, SC_JSON_ARRAY);
    SC_ASSERT_EQ(items->data.array.len, 3);
    sc_json_free(&alloc, val);
}

static void test_json_stringify(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *obj = sc_json_object_new(&alloc);
    sc_json_object_set(&alloc, obj, "name", sc_json_string_new(&alloc, "seaclaw", 7));
    sc_json_object_set(&alloc, obj, "version", sc_json_number_new(&alloc, 1));

    char *out = NULL;
    size_t out_len = 0;
    SC_ASSERT_EQ(sc_json_stringify(&alloc, obj, &out, &out_len), SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_STR_EQ(out, "{\"name\":\"seaclaw\",\"version\":1}");

    alloc.free(alloc.ctx, out, out_len + 1);
    sc_json_free(&alloc, obj);
}

static void test_json_empty_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "{}", 2, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_OBJECT);
    SC_ASSERT_EQ(val->data.object.len, 0);
    sc_json_free(&alloc, val);
}

static void test_json_empty_array(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "[]", 2, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_ARRAY);
    SC_ASSERT_EQ(val->data.array.len, 0);
    sc_json_free(&alloc, val);
}

static void test_json_trailing_comma(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    sc_error_t err = sc_json_parse(&alloc, "[1, 2,]", 7, &val);
    if (err == SC_OK) {
        SC_ASSERT_EQ(val->type, SC_JSON_ARRAY);
        SC_ASSERT_EQ(val->data.array.len, 2);
        sc_json_free(&alloc, val);
    } else {
        SC_ASSERT_EQ(err, SC_ERR_JSON_PARSE);
    }

    val = NULL;
    SC_ASSERT_EQ(sc_json_parse(&alloc, "{\"a\": 1,}", 10, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_OBJECT);
    SC_ASSERT_EQ(val->data.object.len, 1);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val, "a", 0), 1.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_duplicate_key_last_wins(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "{\"x\": 1, \"x\": 2}", 17, &val), SC_OK);
    SC_ASSERT_FLOAT_EQ(sc_json_get_number(val, "x", 0), 2.0, 0.001);
    sc_json_free(&alloc, val);
}

static void test_json_reject_invalid_escape(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_NEQ(sc_json_parse(&alloc, "\"a\\q\"", 5, &val), SC_OK);
    SC_ASSERT_NULL(val);
}

static void test_json_reject_nan_infinity(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_NEQ(sc_json_parse(&alloc, "NaN", 3, &val), SC_OK);
    if (val) { sc_json_free(&alloc, val); val = NULL; }
    SC_ASSERT_NEQ(sc_json_parse(&alloc, "Infinity", 8, &val), SC_OK);
    if (val) { sc_json_free(&alloc, val); val = NULL; }
}

static void test_json_append_helpers(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;

    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_append_string(&buf, "hello", 5), SC_OK);
    SC_ASSERT_STR_EQ(buf.ptr, "\"hello\"");
    sc_json_buf_free(&buf);

    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_append_key_value(&buf, "name", 4, "Alice", 5), SC_OK);
    SC_ASSERT_STR_EQ(buf.ptr, "\"name\":\"Alice\"");
    sc_json_buf_free(&buf);

    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_append_key_int(&buf, "count", 5, 42), SC_OK);
    SC_ASSERT_STR_EQ(buf.ptr, "\"count\":42");
    sc_json_buf_free(&buf);

    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_append_key_bool(&buf, "ok", 2, true), SC_OK);
    SC_ASSERT_STR_EQ(buf.ptr, "\"ok\":true");
    sc_json_buf_free(&buf);
}

static void test_json_unicode_escape(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_value_t *val = NULL;

    SC_ASSERT_EQ(sc_json_parse(&alloc, "\"\\u0000\"", 8, &val), SC_OK);
    SC_ASSERT_EQ(val->type, SC_JSON_STRING);
    SC_ASSERT_EQ(val->data.string.len, 1);
    SC_ASSERT_EQ((unsigned char)val->data.string.ptr[0], 0);
    sc_json_free(&alloc, val);
}

void run_json_tests(void) {
    SC_TEST_SUITE("json");
    SC_RUN_TEST(test_json_parse_null);
    SC_RUN_TEST(test_json_parse_bool);
    SC_RUN_TEST(test_json_parse_number);
    SC_RUN_TEST(test_json_parse_string);
    SC_RUN_TEST(test_json_parse_string_escapes);
    SC_RUN_TEST(test_json_parse_array);
    SC_RUN_TEST(test_json_parse_object);
    SC_RUN_TEST(test_json_parse_nested);
    SC_RUN_TEST(test_json_stringify);
    SC_RUN_TEST(test_json_empty_object);
    SC_RUN_TEST(test_json_empty_array);
    SC_RUN_TEST(test_json_trailing_comma);
    SC_RUN_TEST(test_json_duplicate_key_last_wins);
    SC_RUN_TEST(test_json_reject_invalid_escape);
    SC_RUN_TEST(test_json_reject_nan_infinity);
    SC_RUN_TEST(test_json_append_helpers);
    SC_RUN_TEST(test_json_unicode_escape);
}
