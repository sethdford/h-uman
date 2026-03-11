#include "human/agent/timing.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void timing_bucket_from_samples_computes_percentiles(void) {
    double samples[] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0};
    hu_timing_bucket_t out;
    memset(&out, 0, sizeof(out));

    HU_ASSERT_EQ(hu_timing_bucket_from_samples(samples, 10, &out), HU_OK);
    HU_ASSERT_FLOAT_EQ(out.p10, 10.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.p50, 50.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.p90, 90.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.mean, 55.0, 0.01);
    HU_ASSERT_EQ(out.sample_count, 10u);
}

static void timing_bucket_from_samples_requires_min_5(void) {
    double samples[] = {1.0, 2.0, 3.0};
    hu_timing_bucket_t out;
    memset(&out, 0, sizeof(out));

    HU_ASSERT_EQ(hu_timing_bucket_from_samples(samples, 3, &out), HU_ERR_INVALID_ARGUMENT);
}

static void timing_bucket_from_samples_unsorted_input_still_works(void) {
    double samples[] = {50.0, 10.0, 90.0, 30.0, 70.0};
    hu_timing_bucket_t out;
    memset(&out, 0, sizeof(out));

    HU_ASSERT_EQ(hu_timing_bucket_from_samples(samples, 5, &out), HU_OK);
    HU_ASSERT_FLOAT_EQ(out.p10, 10.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.p25, 30.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.p50, 50.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.p75, 70.0, 0.01);
    HU_ASSERT_FLOAT_EQ(out.p90, 90.0, 0.01);
}

static void timing_model_sample_default_daytime_range(void) {
    uint64_t result = hu_timing_model_sample_default(14, 42);
    HU_ASSERT_TRUE(result >= 5000);
    HU_ASSERT_TRUE(result <= 300000);
}

static void timing_model_sample_default_late_night_range(void) {
    uint64_t result = hu_timing_model_sample_default(3, 123);
    HU_ASSERT_TRUE(result >= 100000);
    HU_ASSERT_TRUE(result <= 5000000);
}

static void timing_model_sample_uses_learned_distribution(void) {
    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));
    model.overall.p10 = 10.0;
    model.overall.p25 = 25.0;
    model.overall.p50 = 50.0;
    model.overall.p75 = 75.0;
    model.overall.p90 = 90.0;
    model.overall.sample_count = 100;

    uint64_t result = hu_timing_model_sample(&model, 14, 3, 50, 999);
    HU_ASSERT_TRUE(result >= (uint64_t)(10.0 * 1000));
    HU_ASSERT_TRUE(result <= (uint64_t)(90.0 * 1000));
}

static void timing_model_sample_falls_back_to_overall(void) {
    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));
    model.overall.p10 = 5.0;
    model.overall.p50 = 30.0;
    model.overall.p90 = 120.0;
    model.overall.sample_count = 50;

    uint64_t result = hu_timing_model_sample(&model, 14, 3, 10, 1);
    HU_ASSERT_TRUE(result >= 5000);
    HU_ASSERT_TRUE(result <= 120000);
}

static void timing_model_sample_falls_back_to_default(void) {
    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));

    uint64_t result = hu_timing_model_sample(&model, 14, 3, 50, 7);
    HU_ASSERT_TRUE(result >= 5000);
    HU_ASSERT_TRUE(result <= 300000);
}

static void timing_adjust_conversation_depth_speeds_up(void) {
    uint64_t base = 100000;
    uint64_t result =
        hu_timing_adjust(base, 10, 0.0, false, 0);
    HU_ASSERT_TRUE(result < base);
}

static void timing_adjust_calendar_busy_slows_down(void) {
    uint64_t base = 100000;
    uint64_t result =
        hu_timing_adjust(base, 0, 0.0, true, 0);
    HU_ASSERT_TRUE(result > base);
}

static void timing_adjust_emotional_intensity_slows_down(void) {
    uint64_t base = 100000;
    uint64_t result =
        hu_timing_adjust(base, 0, 0.9, false, 0);
    HU_ASSERT_TRUE(result > base);
}

static void timing_model_build_query_no_contact(void) {
    char buf[2048];
    size_t len = 0;

    HU_ASSERT_EQ(hu_timing_model_build_query(NULL, 0, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "m1.is_from_me = 0") != NULL);
    /* No contact filter: query ends with text IS NOT NULL, no h.id clause */
    HU_ASSERT_NULL(strstr(buf, "AND h.id = "));
}

static void timing_model_build_query_with_contact(void) {
    char buf[2048];
    size_t len = 0;
    static const char CONTACT[] = "test@example.com";

    HU_ASSERT_EQ(hu_timing_model_build_query(CONTACT, sizeof(CONTACT) - 1, buf, sizeof(buf), &len),
                 HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "h.id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "test@example.com") != NULL);
}

static void timing_model_deinit_frees_contact_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_timing_model_t model;
    memset(&model, 0, sizeof(model));
    model.contact_id = hu_strndup(&alloc, "contact_deinit_test", 19);
    model.contact_id_len = 19;
    HU_ASSERT_NOT_NULL(model.contact_id);

    hu_timing_model_deinit(&alloc, &model);
    HU_ASSERT_NULL(model.contact_id);
    HU_ASSERT_EQ(model.contact_id_len, 0u);
}

void run_timing_tests(void) {
    HU_TEST_SUITE("timing");
    HU_RUN_TEST(timing_bucket_from_samples_computes_percentiles);
    HU_RUN_TEST(timing_bucket_from_samples_requires_min_5);
    HU_RUN_TEST(timing_bucket_from_samples_unsorted_input_still_works);
    HU_RUN_TEST(timing_model_sample_default_daytime_range);
    HU_RUN_TEST(timing_model_sample_default_late_night_range);
    HU_RUN_TEST(timing_model_sample_uses_learned_distribution);
    HU_RUN_TEST(timing_model_sample_falls_back_to_overall);
    HU_RUN_TEST(timing_model_sample_falls_back_to_default);
    HU_RUN_TEST(timing_adjust_conversation_depth_speeds_up);
    HU_RUN_TEST(timing_adjust_calendar_busy_slows_down);
    HU_RUN_TEST(timing_adjust_emotional_intensity_slows_down);
    HU_RUN_TEST(timing_model_build_query_no_contact);
    HU_RUN_TEST(timing_model_build_query_with_contact);
    HU_RUN_TEST(timing_model_deinit_frees_contact_id);
}
