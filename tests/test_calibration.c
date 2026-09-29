#include "human/calibration.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>

static void test_calibration_timing_mock_populates_buckets(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_timing_report_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_EQ(hu_calibration_analyze_timing(&alloc, NULL, NULL, &r), HU_OK);
    HU_ASSERT_EQ(r.by_tod[HU_CALIB_TOD_MORNING].sample_count, 40u);
    HU_ASSERT_TRUE(r.by_tod[HU_CALIB_TOD_MORNING].p50_sec > 100.0);
    HU_ASSERT_EQ(r.active_hours[9], 12u);
    HU_ASSERT_EQ(r.active_hours[14], 20u);
    HU_ASSERT_EQ(r.messages_per_day.sample_count, 30u);
    HU_ASSERT_FLOAT_EQ(r.messages_per_day.p50_sec, 18.0, 0.01);
    hu_timing_report_deinit(&alloc, &r);
}

static void test_calibration_style_mock_populates_metrics(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_style_report_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_EQ(hu_calibration_analyze_style(&alloc, NULL, NULL, &r), HU_OK);
    HU_ASSERT_FLOAT_EQ(r.avg_message_length, 42.0, 0.01);
    HU_ASSERT_FLOAT_EQ(r.emoji_per_message, 0.35, 0.01);
    HU_ASSERT_FLOAT_EQ(r.vocabulary_richness, 0.62, 0.01);
    HU_ASSERT_EQ(r.messages_analyzed, 120u);
    hu_style_report_deinit(&alloc, &r);
}

static void test_calibration_hu_calibrate_mock_returns_persona_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    HU_ASSERT_EQ(hu_calibrate(&alloc, NULL, NULL, NULL, &json), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "\"recommended_overlay\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"channel\":\"auto\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"avg_length\":\"42\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"response_tempo\":\"within_minutes\"") != NULL);
    HU_ASSERT_TRUE(strstr(json, "\"vocabulary_richness\":\"0.620\"") != NULL);
    hu_str_free(&alloc, json);
}

static void test_calibration_hu_calibrate_mock_embeds_explicit_channel(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    HU_ASSERT_EQ(hu_calibrate(&alloc, NULL, NULL, "telegram", &json), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(strstr(json, "\"channel\":\"telegram\"") != NULL);
    hu_str_free(&alloc, json);
}

void run_calibration_tests(void) {
    HU_TEST_SUITE("calibration");
    HU_RUN_TEST(test_calibration_timing_mock_populates_buckets);
    HU_RUN_TEST(test_calibration_style_mock_populates_metrics);
    HU_RUN_TEST(test_calibration_hu_calibrate_mock_returns_persona_json);
    HU_RUN_TEST(test_calibration_hu_calibrate_mock_embeds_explicit_channel);
}
