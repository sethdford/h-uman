#include "human/context.h"
#include "human/core/allocator.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

static void test_context_default_prompt(void) {
    /* Persona-first doctrine (2026-05-17): the default fallback used when no
     * base prompt is supplied MUST NOT announce the model as an AI assistant
     * (that framing was the silent leak path patched alongside this test;
     * see src/agent/context.c). Pin the contract from BOTH directions:
     * the prompt is non-empty AND does not contain the words "AI" or
     * "assistant" in any case. */
    hu_allocator_t alloc = hu_system_allocator();
    char *p = hu_context_build_system_prompt(&alloc, NULL, 0, NULL, 0);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strlen(p) > 0);
    /* Negative contract: no "AI assistant" framing in any casing. */
    HU_ASSERT_TRUE(strstr(p, "AI assistant") == NULL);
    HU_ASSERT_TRUE(strstr(p, "ai assistant") == NULL);
    HU_ASSERT_TRUE(strstr(p, "AI Assistant") == NULL);
    /* Positive contract: instructs the model to respond naturally / in voice. */
    HU_ASSERT_TRUE(strstr(p, "naturally") != NULL || strstr(p, "voice") != NULL);
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_context_custom_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *p = hu_context_build_system_prompt(&alloc, "Be concise.", 11, NULL, 0);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p, "Be concise.");
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_context_format_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_message_t *msgs = NULL;
    size_t count = 99;
    hu_error_t err = hu_context_format_messages(&alloc, NULL, 0, 10, NULL, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void test_context_format_with_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t history[2] = {
        {.role = HU_ROLE_USER,
         .content = "hi",
         .content_len = 2,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_ASSISTANT,
         .content = "hello",
         .content_len = 5,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
    };
    hu_chat_message_t *msgs = NULL;
    size_t count = 0;
    hu_error_t err = hu_context_format_messages(&alloc, history, 2, 10, NULL, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(msgs);
    HU_ASSERT_EQ(msgs[0].role, HU_ROLE_USER);
    HU_ASSERT_EQ(msgs[0].content_len, 2u);
    HU_ASSERT_EQ(msgs[1].role, HU_ROLE_ASSISTANT);
    HU_ASSERT_EQ(msgs[1].content_len, 5u);
    alloc.free(alloc.ctx, msgs, count * sizeof(hu_chat_message_t));
}

static void test_context_format_respects_max_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_owned_message_t history[4] = {
        {.role = HU_ROLE_USER,
         .content = "a",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_ASSISTANT,
         .content = "b",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_USER,
         .content = "c",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
        {.role = HU_ROLE_ASSISTANT,
         .content = "d",
         .content_len = 1,
         .name = NULL,
         .name_len = 0,
         .tool_call_id = NULL,
         .tool_call_id_len = 0,
         .tool_calls = NULL,
         .tool_calls_count = 0},
    };
    hu_chat_message_t *msgs = NULL;
    size_t count = 0;
    hu_error_t err = hu_context_format_messages(&alloc, history, 4, 2, NULL, &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_NOT_NULL(msgs);
    /* Should return last 2 messages (c and d) */
    HU_ASSERT_EQ(msgs[0].content_len, 1u);
    HU_ASSERT_EQ(msgs[0].content[0], 'c');
    HU_ASSERT_EQ(msgs[1].content[0], 'd');
    alloc.free(alloc.ctx, msgs, count * sizeof(hu_chat_message_t));
}

/* ── Multimodal request-body budget (2026-07-25 retry-amplification fix) ──────
 *
 * The estimator and drop helpers below only READ content-part length fields;
 * they never dereference the base64 `data` pointer. That lets these tests
 * simulate a multi-MB image with a 1-byte dummy buffer + a large data_len,
 * pinning the 2KB-not-4MB contract without allocating megabytes. */

static void test_estimate_bytes_counts_image_payload(void) {
    hu_content_part_t part = {0};
    part.tag = HU_CONTENT_PART_IMAGE_BASE64;
    part.data.image_base64.data = "x";         /* not dereferenced by the estimator */
    part.data.image_base64.data_len = 4300000; /* ~4.3 MB base64 image */
    part.data.image_base64.media_type = "image/png";
    part.data.image_base64.media_type_len = 9;

    hu_chat_message_t m = {0};
    m.role = HU_ROLE_USER;
    m.content = "look at this";
    m.content_len = 12;
    m.content_parts = &part;
    m.content_parts_count = 1;

    size_t bytes = hu_chat_message_estimate_bytes(&m);
    /* Pin the bug: the old content_len-only sum was ~12 bytes and let a 4.3 MB
     * image slip past the budget. The estimate MUST reflect the image payload. */
    HU_ASSERT_TRUE(bytes >= 4300000u);
    HU_ASSERT_TRUE(bytes > m.content_len);
    HU_ASSERT_EQ(hu_chat_message_estimate_bytes(NULL), 0u);
}

static void test_drop_oversized_parts_trims_image_keeps_text(void) {
    /* [0] system (text only), [1] user carrying a 4.3 MB image. */
    hu_content_part_t big = {0};
    big.tag = HU_CONTENT_PART_IMAGE_BASE64;
    big.data.image_base64.data = "x";
    big.data.image_base64.data_len = 4300000;
    big.data.image_base64.media_type = "image/png";
    big.data.image_base64.media_type_len = 9;

    hu_chat_message_t msgs[2] = {0};
    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = "sys";
    msgs[0].content_len = 3;
    msgs[1].role = HU_ROLE_USER;
    msgs[1].content = "hey";
    msgs[1].content_len = 3;
    msgs[1].content_parts = &big;
    msgs[1].content_parts_count = 1;

    const size_t cap = (size_t)2 << 20; /* 2 MiB per-message part cap */
    size_t dropped = hu_chat_messages_drop_oversized_parts(msgs, 2, cap);
    HU_ASSERT_EQ(dropped, 1u);
    /* Image dropped from the copy... */
    HU_ASSERT_NULL(msgs[1].content_parts);
    HU_ASSERT_EQ(msgs[1].content_parts_count, 0u);
    /* ...but the text (needed for the turn to make sense) is preserved. */
    HU_ASSERT_EQ(msgs[1].content_len, 3u);
    HU_ASSERT_EQ(msgs[1].content[0], 'h');
    HU_ASSERT_EQ(msgs[0].content_len, 3u); /* system untouched */

    /* Built request is now lean (2KB-not-4MB): sum of estimates well under cap. */
    size_t total =
        hu_chat_message_estimate_bytes(&msgs[0]) + hu_chat_message_estimate_bytes(&msgs[1]);
    HU_ASSERT_TRUE(total < cap);

    /* Retry contract: a transport-failure retry / fallback re-runs assembly.
     * A second pass drops nothing and the body size does not grow. */
    size_t dropped2 = hu_chat_messages_drop_oversized_parts(msgs, 2, cap);
    HU_ASSERT_EQ(dropped2, 0u);
    size_t total2 =
        hu_chat_message_estimate_bytes(&msgs[0]) + hu_chat_message_estimate_bytes(&msgs[1]);
    HU_ASSERT_EQ(total2, total);
}

static void test_drop_oversized_parts_keeps_small_image(void) {
    /* A modest inline image (well under cap) survives — we only drop the
     * pathological multi-MB payloads, not legitimate small attachments. */
    hu_content_part_t small = {0};
    small.tag = HU_CONTENT_PART_IMAGE_BASE64;
    small.data.image_base64.data = "x";
    small.data.image_base64.data_len = 50000; /* ~50 KB */
    small.data.image_base64.media_type = "image/png";
    small.data.image_base64.media_type_len = 9;

    hu_chat_message_t m = {0};
    m.role = HU_ROLE_USER;
    m.content = "pic";
    m.content_len = 3;
    m.content_parts = &small;
    m.content_parts_count = 1;

    size_t dropped = hu_chat_messages_drop_oversized_parts(&m, 1, (size_t)2 << 20);
    HU_ASSERT_EQ(dropped, 0u);
    HU_ASSERT_NOT_NULL(m.content_parts);
    HU_ASSERT_EQ(m.content_parts_count, 1u);
}

void run_context_tests(void) {
    HU_TEST_SUITE("Context");
    HU_RUN_TEST(test_context_default_prompt);
    HU_RUN_TEST(test_context_custom_prompt);
    HU_RUN_TEST(test_context_format_empty);
    HU_RUN_TEST(test_context_format_with_history);
    HU_RUN_TEST(test_context_format_respects_max_messages);
    HU_RUN_TEST(test_estimate_bytes_counts_image_payload);
    HU_RUN_TEST(test_drop_oversized_parts_trims_image_keeps_text);
    HU_RUN_TEST(test_drop_oversized_parts_keeps_small_image);
}
