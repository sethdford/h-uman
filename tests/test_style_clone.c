#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/persona/style_clone.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

static void style_analyze_all_lowercase_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"hello there", "how are you", "that's cool", "sounds good",
                          "see you later", "thanks man", "no problem", "ok cool",
                          "got it", "sure thing"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_FLOAT_EQ(fp.lowercase_ratio, 1.0, 0.01);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_mixed_case_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"hello", "hi there", "howdy", "hey", "yo",
                          "Hello", "Hi there", "Howdy", "Hey", "Yo"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_FLOAT_EQ(fp.lowercase_ratio, 0.5, 0.01);
    HU_ASSERT_FLOAT_EQ(fp.sentence_case_ratio, 0.5, 0.01);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_period_endings(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"one.", "two.", "three.", "four.", "five.",
                          "six",  "seven", "eight", "nine", "ten"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_FLOAT_EQ(fp.period_ending_ratio, 0.5, 0.01);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_laughter_detection_haha(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"haha that's funny", "hahaha yes", "so funny haha",
                          "haha ok", "hahaha", "haha", "haha nice", "hahaha lol",
                          "haha got it", "haha sure"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_TRUE(fp.haha_ratio > 0.9);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_laughter_detection_lol(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"lol that's funny", "lol yes", "so funny lol",
                          "lol ok", "lol", "lol", "lol nice", "lol got it",
                          "lol sure", "lol cool"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_TRUE(fp.lol_ratio > 0.9);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_mixed_laughter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"haha", "haha", "haha", "haha", "haha",
                          "lol",  "lol",  "lol",  "lmao", "lmao"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_TRUE(fp.haha_ratio > 0.4);
    HU_ASSERT_TRUE(fp.lol_ratio > 0.2);
    HU_ASSERT_TRUE(fp.lmao_ratio > 0.1);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_message_length(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"a", "bb", "ccc", "dddd", "eeeee",
                          "ffffff", "ggggggg", "hhhhhhhh", "iiiiiiiii", "jjjjjjjjjj"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    double expected = (1.0 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10) / 10.0;
    HU_ASSERT_FLOAT_EQ(fp.avg_message_length, expected, 0.01);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_emoji_counting(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *emoji = "\xF0\x9F\x98\x80";
    char buf[32];
    memcpy(buf, emoji, 4);
    buf[4] = '\0';
    const char *msgs[] = {buf, buf, buf, "no emoji", "no emoji",
                          "no emoji", "no emoji", "no emoji", "no emoji", "no emoji"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_TRUE(fp.emoji_per_message >= 0.2);
    HU_ASSERT_TRUE(fp.emoji_per_message <= 0.5);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_question_ratio(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"how are you?", "what's up?", "really?",
                          "ok", "cool", "sure", "got it", "nice", "thanks", "bye"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_FLOAT_EQ(fp.question_ratio, 0.3, 0.01);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_requires_min_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"one", "two", "three", "four", "five"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 5, &fp), HU_ERR_INVALID_ARGUMENT);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_analyze_empty_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"hello", "world"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, NULL, 10, &fp), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 0, &fp), HU_ERR_INVALID_ARGUMENT);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_build_query_no_contact(void) {
    char buf[512];
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_style_build_query(NULL, 0, buf, sizeof(buf), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "handle_id") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "is_from_me = 1") != NULL);
}

static void style_build_query_with_contact(void) {
    char buf[512];
    size_t out_len = 0;
    const char *contact = "+1234567890";
    size_t contact_len = strlen(contact);

    HU_ASSERT_EQ(hu_style_build_query(contact, contact_len, buf, sizeof(buf), &out_len),
                 HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "handle_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "handle") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "+1234567890") != NULL);
}

static void style_build_query_escapes_quotes(void) {
    char buf[512];
    size_t out_len = 0;
    const char *contact = "O'Brien";
    size_t contact_len = strlen(contact);

    HU_ASSERT_EQ(hu_style_build_query(contact, contact_len, buf, sizeof(buf), &out_len),
                 HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
}

static void style_fingerprint_to_prompt_produces_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    fp.lowercase_ratio = 0.87;
    fp.period_ending_ratio = 0.12;
    fp.ellipsis_ratio = 0.08;
    fp.haha_ratio = 0.72;
    fp.lol_ratio = 0.20;
    fp.lmao_ratio = 0.08;
    fp.avg_message_length = 34.0;
    fp.emoji_per_message = 0.3;
    fp.double_text_ratio = 0.15;
    fp.sample_count = 100;

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_style_fingerprint_to_prompt(&alloc, &fp, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "lowercase") != NULL);
    HU_ASSERT_TRUE(strstr(out, "haha") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_style_fingerprint_deinit(&alloc, &fp);
}

static void style_fingerprint_deinit_frees_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    fp.contact_id = hu_strndup(&alloc, "test_contact", 12);
    fp.contact_id_len = 12;

    hu_style_fingerprint_deinit(&alloc, &fp);
    HU_ASSERT_NULL(fp.contact_id);
    HU_ASSERT_EQ(fp.contact_id_len, 0u);
}

static void style_analyze_double_text_detection(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"msg1", "msg2", "msg3", "msg4", "msg5",
                          "msg6", "msg7", "msg8", "msg9", "msg10"};
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    HU_ASSERT_EQ(hu_style_analyze_messages(&alloc, msgs, 10, &fp), HU_OK);
    HU_ASSERT_TRUE(fp.double_text_ratio > 0.0);

    hu_style_fingerprint_deinit(&alloc, &fp);
}

void run_style_clone_tests(void) {
    HU_TEST_SUITE("style_clone");
    HU_RUN_TEST(style_analyze_all_lowercase_messages);
    HU_RUN_TEST(style_analyze_mixed_case_messages);
    HU_RUN_TEST(style_analyze_period_endings);
    HU_RUN_TEST(style_analyze_laughter_detection_haha);
    HU_RUN_TEST(style_analyze_laughter_detection_lol);
    HU_RUN_TEST(style_analyze_mixed_laughter);
    HU_RUN_TEST(style_analyze_message_length);
    HU_RUN_TEST(style_analyze_emoji_counting);
    HU_RUN_TEST(style_analyze_question_ratio);
    HU_RUN_TEST(style_analyze_requires_min_messages);
    HU_RUN_TEST(style_analyze_empty_input);
    HU_RUN_TEST(style_build_query_no_contact);
    HU_RUN_TEST(style_build_query_with_contact);
    HU_RUN_TEST(style_build_query_escapes_quotes);
    HU_RUN_TEST(style_fingerprint_to_prompt_produces_output);
    HU_RUN_TEST(style_fingerprint_deinit_frees_memory);
    HU_RUN_TEST(style_analyze_double_text_detection);
}
