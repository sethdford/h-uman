#include "seaclaw/core/allocator.h"
#include "seaclaw/memory/fast_capture.h"
#include "seaclaw/memory/stm.h"
#include "test_framework.h"
#include <string.h>

static void fc_detects_relationship_person(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    const char *text = "I talked to my mom today";
    sc_error_t err = sc_fast_capture(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.entity_count, 1);
    SC_ASSERT_STR_EQ(out.entities[0].name, "mom");
    SC_ASSERT_STR_EQ(out.entities[0].type, "person");
    sc_fc_result_deinit(&out, &alloc);
}

static void fc_detects_emotion_frustration(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    const char *text = "I'm so frustrated with this";
    sc_error_t err = sc_fast_capture(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.emotion_count, 1);
    SC_ASSERT_EQ(out.emotions[0].tag, SC_EMOTION_FRUSTRATION);
    sc_fc_result_deinit(&out, &alloc);
}

static void fc_detects_work_topic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    const char *text = "Things at work are crazy";
    sc_error_t err = sc_fast_capture(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out.primary_topic);
    SC_ASSERT_STR_EQ(out.primary_topic, "work");
    sc_fc_result_deinit(&out, &alloc);
}

static void fc_detects_commitment(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    const char *text = "I'll finish the report by Friday";
    sc_error_t err = sc_fast_capture(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.has_commitment);
    sc_fc_result_deinit(&out, &alloc);
}

static void fc_detects_question(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    const char *text = "What should I do?";
    sc_error_t err = sc_fast_capture(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.has_question);
    sc_fc_result_deinit(&out, &alloc);
}

static void fc_handles_empty_text(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    sc_error_t err = sc_fast_capture(&alloc, "", 0, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.entity_count, 0);
    SC_ASSERT_EQ(out.emotion_count, 0);
    SC_ASSERT_NULL(out.primary_topic);
    SC_ASSERT_FALSE(out.has_commitment);
    SC_ASSERT_FALSE(out.has_question);
    sc_fc_result_deinit(&out, &alloc);
}

static void fc_detects_multiple_emotions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_fc_result_t out;
    const char *text = "I'm excited but also anxious";
    sc_error_t err = sc_fast_capture(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(out.emotion_count >= 2);
    bool has_joy = false, has_anxiety = false;
    for (size_t i = 0; i < out.emotion_count; i++) {
        if (out.emotions[i].tag == SC_EMOTION_JOY || out.emotions[i].tag == SC_EMOTION_EXCITEMENT)
            has_joy = true;
        if (out.emotions[i].tag == SC_EMOTION_ANXIETY)
            has_anxiety = true;
    }
    SC_ASSERT_TRUE(has_joy);
    SC_ASSERT_TRUE(has_anxiety);
    sc_fc_result_deinit(&out, &alloc);
}

void run_fast_capture_tests(void) {
    SC_TEST_SUITE("fast_capture");
    SC_RUN_TEST(fc_detects_relationship_person);
    SC_RUN_TEST(fc_detects_emotion_frustration);
    SC_RUN_TEST(fc_detects_work_topic);
    SC_RUN_TEST(fc_detects_commitment);
    SC_RUN_TEST(fc_detects_question);
    SC_RUN_TEST(fc_handles_empty_text);
    SC_RUN_TEST(fc_detects_multiple_emotions);
}
