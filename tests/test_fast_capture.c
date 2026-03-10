#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fast_capture.h"
#include "human/memory/stm.h"
#include "test_framework.h"
#include <string.h>

static void fc_detects_relationship_person(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "I talked to my mom today";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.entity_count, 1);
    HU_ASSERT_STR_EQ(out.entities[0].name, "mom");
    HU_ASSERT_STR_EQ(out.entities[0].type, "person");
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_detects_emotion_frustration(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "I'm so frustrated with this";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.emotion_count, 1);
    HU_ASSERT_EQ(out.emotions[0].tag, HU_EMOTION_FRUSTRATION);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_detects_work_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "Things at work are crazy";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out.primary_topic);
    HU_ASSERT_STR_EQ(out.primary_topic, "work");
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_detects_commitment(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "I'll finish the report by Friday";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.has_commitment);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_detects_question(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "What should I do?";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.has_question);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_handles_empty_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    hu_error_t err = hu_fast_capture(&alloc, "", 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.entity_count, 0);
    HU_ASSERT_EQ(out.emotion_count, 0);
    HU_ASSERT_NULL(out.primary_topic);
    HU_ASSERT_FALSE(out.has_commitment);
    HU_ASSERT_FALSE(out.has_question);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_detects_multiple_emotions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "I'm excited but also anxious";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.emotion_count >= 2);
    bool has_joy = false, has_anxiety = false;
    for (size_t i = 0; i < out.emotion_count; i++) {
        if (out.emotions[i].tag == HU_EMOTION_JOY || out.emotions[i].tag == HU_EMOTION_EXCITEMENT)
            has_joy = true;
        if (out.emotions[i].tag == HU_EMOTION_ANXIETY)
            has_anxiety = true;
    }
    HU_ASSERT_TRUE(has_joy);
    HU_ASSERT_TRUE(has_anxiety);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_null_alloc_fails(void) {
    hu_fc_result_t result;
    hu_error_t err = hu_fast_capture(NULL, "hello", 5, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void fc_very_long_text_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    char buf[10001];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';

    hu_error_t err = hu_fast_capture(&alloc, buf, sizeof(buf) - 1, &out);
    HU_ASSERT_EQ(err, HU_OK);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_multiple_commitments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_t out;
    const char *text = "I will do A and I'll do B";
    hu_error_t err = hu_fast_capture(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.has_commitment);
    hu_fc_result_deinit(&out, &alloc);
}

static void fc_result_deinit_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_fc_result_deinit(NULL, &alloc);
}

void run_fast_capture_tests(void) {
    HU_TEST_SUITE("fast_capture");
    HU_RUN_TEST(fc_detects_relationship_person);
    HU_RUN_TEST(fc_detects_emotion_frustration);
    HU_RUN_TEST(fc_detects_work_topic);
    HU_RUN_TEST(fc_detects_commitment);
    HU_RUN_TEST(fc_detects_question);
    HU_RUN_TEST(fc_handles_empty_text);
    HU_RUN_TEST(fc_detects_multiple_emotions);
    HU_RUN_TEST(fc_null_alloc_fails);
    HU_RUN_TEST(fc_very_long_text_no_crash);
    HU_RUN_TEST(fc_multiple_commitments);
    HU_RUN_TEST(fc_result_deinit_null_safe);
}
