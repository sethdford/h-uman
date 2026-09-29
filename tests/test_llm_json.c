/* tests/test_llm_json.c — contract tests for hu_llm_json_locate.
 *
 * Each case is a reply shape observed live on :8741 (GLM-4.5-Air) or in the
 * 2026-09-04 service-loop log: bare object, ```json fence, <think> block
 * followed by a fence, prose on both sides, truncated payload. The locator
 * must find the payload in the first four and refuse the last. */

#include "human/util/llm_json.h"
#include "test_framework.h"

#include <string.h>

static void locate_eq(const char *reply, const char *expected) {
    const char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT(hu_llm_json_locate(reply, strlen(reply), &out, &out_len));
    HU_ASSERT_EQ(out_len, strlen(expected));
    HU_ASSERT_EQ(memcmp(out, expected, out_len), 0);
}

static void test_llm_json_bare_object(void) {
    locate_eq("{\"facts\":[]}", "{\"facts\":[]}");
}

static void test_llm_json_strips_markdown_fence(void) {
    /* Verbatim shape from the live probe on 2026-09-04. */
    locate_eq("```json\n{\"facts\":[{\"key\":\"person\",\"value\":\"mindy\"}]}\n```",
              "{\"facts\":[{\"key\":\"person\",\"value\":\"mindy\"}]}");
}

static void test_llm_json_skips_think_block_then_fence(void) {
    locate_eq("<think>the user wants JSON, {not this}</think>\n```json\n{\"a\":1}\n```",
              "{\"a\":1}");
}

static void test_llm_json_skips_thought_block_case_insensitive(void) {
    locate_eq("<Thought>[irrelevant]</THOUGHT> {\"b\":[1,2]}", "{\"b\":[1,2]}");
}

static void test_llm_json_prose_both_sides(void) {
    locate_eq("Sure! Here is the object:\n{\"patterns\":[],\"prose_summary\":\"x\"}\nLet me "
              "know if you need more.",
              "{\"patterns\":[],\"prose_summary\":\"x\"}");
}

static void test_llm_json_array_payload(void) {
    locate_eq("[{\"subject\":\"user\"}]", "[{\"subject\":\"user\"}]");
}

static void test_llm_json_brackets_inside_strings_do_not_count(void) {
    locate_eq("{\"s\":\"a } b ] \\\" c\",\"n\":1}", "{\"s\":\"a } b ] \\\" c\",\"n\":1}");
}

static void test_llm_json_truncated_payload_is_not_found(void) {
    const char *out = NULL;
    size_t out_len = 0;
    const char *truncated = "{\"patterns\":[{\"type\":\"x\",\"observation\":\"cut";
    HU_ASSERT(!hu_llm_json_locate(truncated, strlen(truncated), &out, &out_len));
}

static void test_llm_json_unclosed_think_is_not_found(void) {
    const char *out = NULL;
    size_t out_len = 0;
    const char *s = "<think>still thinking about {\"a\":1}";
    HU_ASSERT(!hu_llm_json_locate(s, strlen(s), &out, &out_len));
}

static void test_llm_json_rejects_empty_and_null(void) {
    const char *out = (const char *)1;
    size_t out_len = 7;
    HU_ASSERT(!hu_llm_json_locate(NULL, 0, &out, &out_len));
    HU_ASSERT(!hu_llm_json_locate("", 0, &out, &out_len));
    HU_ASSERT(!hu_llm_json_locate("no json here", 12, &out, &out_len));
    HU_ASSERT(out == NULL);
    HU_ASSERT_EQ(out_len, (size_t)0);
}

void run_llm_json_tests(void) {
    HU_TEST_SUITE("llm_json");
    HU_RUN_TEST(test_llm_json_bare_object);
    HU_RUN_TEST(test_llm_json_strips_markdown_fence);
    HU_RUN_TEST(test_llm_json_skips_think_block_then_fence);
    HU_RUN_TEST(test_llm_json_skips_thought_block_case_insensitive);
    HU_RUN_TEST(test_llm_json_prose_both_sides);
    HU_RUN_TEST(test_llm_json_array_payload);
    HU_RUN_TEST(test_llm_json_brackets_inside_strings_do_not_count);
    HU_RUN_TEST(test_llm_json_truncated_payload_is_not_found);
    HU_RUN_TEST(test_llm_json_unclosed_think_is_not_found);
    HU_RUN_TEST(test_llm_json_rejects_empty_and_null);
}
