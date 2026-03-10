#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "test_framework.h"
#include <string.h>

static void test_json_parse_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, "null", 4, &val);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(val);
    HU_ASSERT_EQ(val->type, HU_JSON_NULL);
    hu_json_free(&alloc, val);
}

static void test_json_parse_bool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "true", 4, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_BOOL);
    HU_ASSERT_TRUE(val->data.boolean);
    hu_json_free(&alloc, val);

    HU_ASSERT_EQ(hu_json_parse(&alloc, "false", 5, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_BOOL);
    HU_ASSERT_FALSE(val->data.boolean);
    hu_json_free(&alloc, val);
}

static void test_json_parse_number(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "42", 2, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_NUMBER);
    HU_ASSERT_FLOAT_EQ(val->data.number, 42.0, 0.001);
    hu_json_free(&alloc, val);

    HU_ASSERT_EQ(hu_json_parse(&alloc, "-3.14", 5, &val), HU_OK);
    HU_ASSERT_FLOAT_EQ(val->data.number, -3.14, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_parse_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"hello\"", 7, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_STRING);
    HU_ASSERT_STR_EQ(val->data.string.ptr, "hello");
    HU_ASSERT_EQ(val->data.string.len, 5);
    hu_json_free(&alloc, val);
}

static void test_json_parse_string_escapes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"a\\nb\\tc\"", 9, &val), HU_OK);
    HU_ASSERT_STR_EQ(val->data.string.ptr, "a\nb\tc");
    hu_json_free(&alloc, val);
}

static void test_json_parse_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    const char *input = "[1, \"two\", true, null]";

    HU_ASSERT_EQ(hu_json_parse(&alloc, input, strlen(input), &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(val->data.array.len, 4);
    HU_ASSERT_EQ(val->data.array.items[0]->type, HU_JSON_NUMBER);
    HU_ASSERT_EQ(val->data.array.items[1]->type, HU_JSON_STRING);
    HU_ASSERT_EQ(val->data.array.items[2]->type, HU_JSON_BOOL);
    HU_ASSERT_EQ(val->data.array.items[3]->type, HU_JSON_NULL);
    hu_json_free(&alloc, val);
}

static void test_json_parse_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    const char *input = "{\"name\": \"human\", \"version\": 1, \"active\": true}";

    HU_ASSERT_EQ(hu_json_parse(&alloc, input, strlen(input), &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_OBJECT);
    HU_ASSERT_STR_EQ(hu_json_get_string(val, "name"), "human");
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val, "version", 0), 1.0, 0.001);
    HU_ASSERT_TRUE(hu_json_get_bool(val, "active", false));
    hu_json_free(&alloc, val);
}

static void test_json_parse_nested(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;
    const char *input = "{\"data\": {\"items\": [1, 2, 3]}}";

    HU_ASSERT_EQ(hu_json_parse(&alloc, input, strlen(input), &val), HU_OK);
    hu_json_value_t *data = hu_json_object_get(val, "data");
    HU_ASSERT_NOT_NULL(data);
    hu_json_value_t *items = hu_json_object_get(data, "items");
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_EQ(items->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(items->data.array.len, 3);
    hu_json_free(&alloc, val);
}

static void test_json_stringify(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "name", hu_json_string_new(&alloc, "human", 5));
    hu_json_object_set(&alloc, obj, "version", hu_json_number_new(&alloc, 1));

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_json_stringify(&alloc, obj, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "{\"name\":\"human\",\"version\":1}");
    HU_ASSERT_EQ(out_len, strlen(out));

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_json_free(&alloc, obj);
}

static void test_json_stringify_null_val_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_json_stringify(&alloc, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_json_stringify_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *obj = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, obj, "a", hu_json_number_new(&alloc, 1));
    size_t out_len = 0;
    hu_error_t err = hu_json_stringify(&alloc, obj, NULL, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_json_free(&alloc, obj);
}

static void test_json_empty_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "{}", 2, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_OBJECT);
    HU_ASSERT_EQ(val->data.object.len, 0);
    hu_json_free(&alloc, val);
}

static void test_json_empty_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "[]", 2, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(val->data.array.len, 0);
    hu_json_free(&alloc, val);
}

static void test_json_trailing_comma(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    hu_error_t err = hu_json_parse(&alloc, "[1, 2,]", 7, &val);
    if (err == HU_OK) {
        HU_ASSERT_EQ(val->type, HU_JSON_ARRAY);
        HU_ASSERT_EQ(val->data.array.len, 2);
        hu_json_free(&alloc, val);
    } else {
        HU_ASSERT_EQ(err, HU_ERR_JSON_PARSE);
    }

    val = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, "{\"a\": 1,}", 10, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_OBJECT);
    HU_ASSERT_EQ(val->data.object.len, 1);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val, "a", 0), 1.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_duplicate_key_last_wins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "{\"x\": 1, \"x\": 2}", 17, &val), HU_OK);
    HU_ASSERT_FLOAT_EQ(hu_json_get_number(val, "x", 0), 2.0, 0.001);
    hu_json_free(&alloc, val);
}

static void test_json_reject_invalid_escape(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_NEQ(hu_json_parse(&alloc, "\"a\\q\"", 5, &val), HU_OK);
    HU_ASSERT_NULL(val);
}

static void test_json_reject_nan_infinity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_NEQ(hu_json_parse(&alloc, "NaN", 3, &val), HU_OK);
    if (val) {
        hu_json_free(&alloc, val);
        val = NULL;
    }
    HU_ASSERT_NEQ(hu_json_parse(&alloc, "Infinity", 8, &val), HU_OK);
    if (val) {
        hu_json_free(&alloc, val);
        val = NULL;
    }
}

static void test_json_append_helpers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;

    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_append_string(&buf, "hello", 5), HU_OK);
    HU_ASSERT_STR_EQ(buf.ptr, "\"hello\"");
    hu_json_buf_free(&buf);

    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_append_key_value(&buf, "name", 4, "Alice", 5), HU_OK);
    HU_ASSERT_STR_EQ(buf.ptr, "\"name\":\"Alice\"");
    hu_json_buf_free(&buf);

    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_append_key_int(&buf, "count", 5, 42), HU_OK);
    HU_ASSERT_STR_EQ(buf.ptr, "\"count\":42");
    hu_json_buf_free(&buf);

    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_append_key_bool(&buf, "ok", 2, true), HU_OK);
    HU_ASSERT_STR_EQ(buf.ptr, "\"ok\":true");
    hu_json_buf_free(&buf);
}

static void test_json_unicode_escape(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *val = NULL;

    HU_ASSERT_EQ(hu_json_parse(&alloc, "\"\\u0000\"", 8, &val), HU_OK);
    HU_ASSERT_EQ(val->type, HU_JSON_STRING);
    HU_ASSERT_EQ(val->data.string.len, 1);
    HU_ASSERT_EQ((unsigned char)val->data.string.ptr[0], 0);
    hu_json_free(&alloc, val);
}

void run_json_tests(void) {
    HU_TEST_SUITE("json");
    HU_RUN_TEST(test_json_parse_null);
    HU_RUN_TEST(test_json_parse_bool);
    HU_RUN_TEST(test_json_parse_number);
    HU_RUN_TEST(test_json_parse_string);
    HU_RUN_TEST(test_json_parse_string_escapes);
    HU_RUN_TEST(test_json_parse_array);
    HU_RUN_TEST(test_json_parse_object);
    HU_RUN_TEST(test_json_parse_nested);
    HU_RUN_TEST(test_json_stringify);
    HU_RUN_TEST(test_json_stringify_null_val_returns_error);
    HU_RUN_TEST(test_json_stringify_null_out_returns_error);
    HU_RUN_TEST(test_json_empty_object);
    HU_RUN_TEST(test_json_empty_array);
    HU_RUN_TEST(test_json_trailing_comma);
    HU_RUN_TEST(test_json_duplicate_key_last_wins);
    HU_RUN_TEST(test_json_reject_invalid_escape);
    HU_RUN_TEST(test_json_reject_nan_infinity);
    HU_RUN_TEST(test_json_append_helpers);
    HU_RUN_TEST(test_json_unicode_escape);
}
