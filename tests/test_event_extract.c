#include "human/context/event_extract.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void event_extract_day_reference(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "my interview is on Tuesday";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 1);
    HU_ASSERT_NOT_NULL(out.events[0].description);
    HU_ASSERT_TRUE(strstr(out.events[0].description, "interview") != NULL);
    HU_ASSERT_STR_EQ(out.events[0].temporal_ref, "Tuesday");
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_tomorrow(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I have a meeting tomorrow";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 1);
    HU_ASSERT_STR_EQ(out.events[0].description, "meeting");
    HU_ASSERT_STR_EQ(out.events[0].temporal_ref, "tomorrow");
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_next_week(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "birthday party next week";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 1);
    HU_ASSERT_STR_EQ(out.events[0].description, "birthday party");
    HU_ASSERT_STR_EQ(out.events[0].temporal_ref, "next week");
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_date(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "doctor appointment on March 15th";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 1);
    HU_ASSERT_STR_EQ(out.events[0].description, "doctor appointment");
    HU_ASSERT_NOT_NULL(out.events[0].temporal_ref);
    HU_ASSERT_TRUE(strstr(out.events[0].temporal_ref, "March 15th") != NULL);
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_no_events(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "hello how are you";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 0);
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_null_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, NULL, 0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 0);
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_null_alloc(void) {
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(NULL, "text", 4, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void event_extract_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_event_extract(&alloc, "text", 4, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void event_extract_multiple_events(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "meeting Tuesday, birthday party next week";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 2);
    HU_ASSERT_TRUE(strstr(out.events[0].description, "meeting") != NULL);
    HU_ASSERT_STR_EQ(out.events[0].temporal_ref, "Tuesday");
    HU_ASSERT_TRUE(strstr(out.events[1].description, "birthday") != NULL);
    HU_ASSERT_STR_EQ(out.events[1].temporal_ref, "next week");
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_in_two_weeks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "I have a meeting in 2 weeks";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 1);
    HU_ASSERT_STR_EQ(out.events[0].description, "meeting");
    HU_ASSERT_STR_EQ(out.events[0].temporal_ref, "in 2 weeks");
    hu_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_deinit_partial(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *text = "meeting tomorrow";
    hu_event_extract_result_t out;
    hu_error_t err = hu_event_extract(&alloc, text, strlen(text), &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.event_count, 1);
    hu_event_extract_result_deinit(&out, &alloc);
}

void run_event_extract_tests(void) {
    HU_TEST_SUITE("event_extract");
    HU_RUN_TEST(event_extract_day_reference);
    HU_RUN_TEST(event_extract_tomorrow);
    HU_RUN_TEST(event_extract_next_week);
    HU_RUN_TEST(event_extract_date);
    HU_RUN_TEST(event_extract_no_events);
    HU_RUN_TEST(event_extract_null_input);
    HU_RUN_TEST(event_extract_null_alloc);
    HU_RUN_TEST(event_extract_null_out);
    HU_RUN_TEST(event_extract_multiple_events);
    HU_RUN_TEST(event_extract_in_two_weeks);
    HU_RUN_TEST(event_extract_deinit_partial);
}
