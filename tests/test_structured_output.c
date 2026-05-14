#include "human/core/allocator.h"
#include "human/provider/structured_output.h"
#include "test_framework.h"
#include <string.h>

static void schema_returns_non_null_non_empty(void) {
    const char *schema = hu_structured_output_chat_reply_schema();
    HU_ASSERT_NOT_NULL(schema);
    HU_ASSERT(hu_structured_output_chat_reply_schema_len() > 0);
    HU_ASSERT_EQ(strlen(schema), hu_structured_output_chat_reply_schema_len());
}

static void extract_reply_returns_reply_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "{\"reply\":\"hello\"}";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err = hu_structured_output_extract_reply(&alloc, body, sizeof(body) - 1, &reply,
                                                        &reply_len, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(reply);
    HU_ASSERT_STR_EQ(reply, "hello");
    HU_ASSERT_EQ(reply_len, (size_t)5);
    alloc.free(alloc.ctx, reply, reply_len + 1);
}

static void extract_reply_with_reasoning_preserves_both(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "{\"reply\":\"hi there\",\"reasoning\":\"internal thought\"}";
    char *reply = NULL;
    size_t reply_len = 0;
    char *reasoning = NULL;
    size_t reasoning_len = 0;
    hu_error_t err = hu_structured_output_extract_reply(&alloc, body, sizeof(body) - 1, &reply,
                                                        &reply_len, &reasoning, &reasoning_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(reply);
    HU_ASSERT_STR_EQ(reply, "hi there");
    HU_ASSERT_NOT_NULL(reasoning);
    HU_ASSERT_STR_EQ(reasoning, "internal thought");
    alloc.free(alloc.ctx, reply, reply_len + 1);
    alloc.free(alloc.ctx, reasoning, reasoning_len + 1);
}

static void extract_reply_malformed_json_returns_parse_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "not json at all";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err = hu_structured_output_extract_reply(&alloc, body, sizeof(body) - 1, &reply,
                                                        &reply_len, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    HU_ASSERT(reply == NULL);
}

static void sentinel_extract_finds_reply_markers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "some preamble <REPLY>foo</REPLY> trailing";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err =
        hu_structured_output_extract_sentinel(&alloc, body, sizeof(body) - 1, &reply, &reply_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(reply);
    HU_ASSERT_STR_EQ(reply, "foo");
    HU_ASSERT_EQ(reply_len, (size_t)3);
    alloc.free(alloc.ctx, reply, reply_len + 1);
}

static void sentinel_extract_without_markers_returns_parse_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char body[] = "no markers here";
    char *reply = NULL;
    size_t reply_len = 0;
    hu_error_t err =
        hu_structured_output_extract_sentinel(&alloc, body, sizeof(body) - 1, &reply, &reply_len);
    HU_ASSERT_EQ(err, HU_ERR_PARSE);
    HU_ASSERT(reply == NULL);
}

void run_structured_output_tests(void) {
    HU_TEST_SUITE("structured_output");
    HU_RUN_TEST(schema_returns_non_null_non_empty);
    HU_RUN_TEST(extract_reply_returns_reply_field);
    HU_RUN_TEST(extract_reply_with_reasoning_preserves_both);
    HU_RUN_TEST(extract_reply_malformed_json_returns_parse_error);
    HU_RUN_TEST(sentinel_extract_finds_reply_markers);
    HU_RUN_TEST(sentinel_extract_without_markers_returns_parse_error);
}
