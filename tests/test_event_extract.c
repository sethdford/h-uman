#include "seaclaw/context/event_extract.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void event_extract_day_reference(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "my interview is on Tuesday";
    sc_event_extract_result_t out;
    sc_error_t err = sc_event_extract(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.event_count, 1);
    SC_ASSERT_NOT_NULL(out.events[0].description);
    SC_ASSERT_TRUE(strstr(out.events[0].description, "interview") != NULL);
    SC_ASSERT_STR_EQ(out.events[0].temporal_ref, "Tuesday");
    sc_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_tomorrow(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "I have a meeting tomorrow";
    sc_event_extract_result_t out;
    sc_error_t err = sc_event_extract(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.event_count, 1);
    SC_ASSERT_STR_EQ(out.events[0].description, "meeting");
    SC_ASSERT_STR_EQ(out.events[0].temporal_ref, "tomorrow");
    sc_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_next_week(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "birthday party next week";
    sc_event_extract_result_t out;
    sc_error_t err = sc_event_extract(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.event_count, 1);
    SC_ASSERT_STR_EQ(out.events[0].description, "birthday party");
    SC_ASSERT_STR_EQ(out.events[0].temporal_ref, "next week");
    sc_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_date(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "doctor appointment on March 15th";
    sc_event_extract_result_t out;
    sc_error_t err = sc_event_extract(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.event_count, 1);
    SC_ASSERT_STR_EQ(out.events[0].description, "doctor appointment");
    SC_ASSERT_NOT_NULL(out.events[0].temporal_ref);
    SC_ASSERT_TRUE(strstr(out.events[0].temporal_ref, "March 15th") != NULL);
    sc_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_no_events(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *text = "hello how are you";
    sc_event_extract_result_t out;
    sc_error_t err = sc_event_extract(&alloc, text, strlen(text), &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.event_count, 0);
    sc_event_extract_result_deinit(&out, &alloc);
}

static void event_extract_null_input(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_event_extract_result_t out;
    sc_error_t err = sc_event_extract(&alloc, NULL, 0, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.event_count, 0);
    sc_event_extract_result_deinit(&out, &alloc);
}

void run_event_extract_tests(void) {
    SC_TEST_SUITE("event_extract");
    SC_RUN_TEST(event_extract_day_reference);
    SC_RUN_TEST(event_extract_tomorrow);
    SC_RUN_TEST(event_extract_next_week);
    SC_RUN_TEST(event_extract_date);
    SC_RUN_TEST(event_extract_no_events);
    SC_RUN_TEST(event_extract_null_input);
}
