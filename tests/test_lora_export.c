/* tests/test_lora_export.c
 *
 * Sprint B C-loop — collector → JSONL exporter.
 * Contracts (8 tests):
 *   1. json_escape: plain ASCII passes through unchanged
 *   2. json_escape: quotes escaped to \"
 *   3. json_escape: backslashes escaped to \\
 *   4. json_escape: newlines/tabs escaped to \n/\t
 *   5. json_escape: control chars → \uXXXX
 *   6. render_jsonl_line: with rejected → DPO shape
 *   7. render_jsonl_line: without rejected → SFT shape
 *   8. render_jsonl_line: empty prompt or chosen → 0 (drop unusable)
 *   9. export_dpo_pairs: NOT_SUPPORTED in test builds (no SQLite path)
 */

#include "human/ml/lora_export.h"

#include "test_framework.h"

#include <string.h>

static void test_json_escape_plain_ascii_passthrough(void) {
    char buf[64] = {0};
    size_t n = hu_lora_export_json_escape("hello world", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "hello world");
}

static void test_json_escape_quotes(void) {
    char buf[64] = {0};
    hu_lora_export_json_escape("she said \"hi\"", buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "\\\"hi\\\"") != NULL);
}

static void test_json_escape_backslashes(void) {
    char buf[64] = {0};
    hu_lora_export_json_escape("path\\to\\file", buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "\\\\to\\\\file") != NULL);
}

static void test_json_escape_newlines_tabs(void) {
    char buf[64] = {0};
    hu_lora_export_json_escape("line1\nline2\tcol", buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "line1\\nline2\\tcol") != NULL);
}

static void test_json_escape_control_chars(void) {
    char buf[64] = {0};
    /* 0x01 control char →  */
    char input[] = {'a', 0x01, 'b', '\0'};
    hu_lora_export_json_escape(input, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "\\u0001") != NULL);
}

static void test_render_jsonl_with_rejected_dpo_shape(void) {
    hu_lora_export_pair_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.prompt, sizeof(p.prompt), "What's up?");
    snprintf(p.chosen, sizeof(p.chosen), "Sup!");
    snprintf(p.rejected, sizeof(p.rejected), "Greetings.");
    p.timestamp = 1700000000;
    char buf[512] = {0};
    size_t n = hu_lora_export_render_jsonl_line(&p, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "\"prompt\":\"What's up?\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"chosen\":\"Sup!\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"rejected\":\"Greetings.\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"ts\":1700000000") != NULL);
}

static void test_render_jsonl_without_rejected_sft_shape(void) {
    hu_lora_export_pair_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.prompt, sizeof(p.prompt), "ping");
    snprintf(p.chosen, sizeof(p.chosen), "pong");
    p.timestamp = 1700000000;
    char buf[512] = {0};
    hu_lora_export_render_jsonl_line(&p, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "\"chosen\":\"pong\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"rejected\"") == NULL);
}

static void test_render_jsonl_drops_unusable(void) {
    hu_lora_export_pair_t p;
    memset(&p, 0, sizeof(p));
    /* No prompt → drop. */
    snprintf(p.chosen, sizeof(p.chosen), "answer");
    char buf[256] = {0};
    HU_ASSERT_EQ((int)hu_lora_export_render_jsonl_line(&p, buf, sizeof(buf)), 0);
    /* No chosen → drop. */
    memset(&p, 0, sizeof(p));
    snprintf(p.prompt, sizeof(p.prompt), "q");
    HU_ASSERT_EQ((int)hu_lora_export_render_jsonl_line(&p, buf, sizeof(buf)), 0);
}

static void test_export_returns_not_supported_in_test_build(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_lora_export_dpo_pairs(&alloc, "/tmp/x.db", "/tmp/x.jsonl", 0, &count);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
}

void run_lora_export_tests(void) {
    HU_TEST_SUITE("lora_export");
    HU_RUN_TEST(test_json_escape_plain_ascii_passthrough);
    HU_RUN_TEST(test_json_escape_quotes);
    HU_RUN_TEST(test_json_escape_backslashes);
    HU_RUN_TEST(test_json_escape_newlines_tabs);
    HU_RUN_TEST(test_json_escape_control_chars);
    HU_RUN_TEST(test_render_jsonl_with_rejected_dpo_shape);
    HU_RUN_TEST(test_render_jsonl_without_rejected_sft_shape);
    HU_RUN_TEST(test_render_jsonl_drops_unusable);
    HU_RUN_TEST(test_export_returns_not_supported_in_test_build);
}
