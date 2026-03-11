#ifdef HU_ENABLE_FEEDS

#include "human/core/allocator.h"
#include "human/feeds/apple.h"
#include "test_framework.h"
#include <string.h>

static void apple_photos_fetch_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_apple_photos_fetch(&alloc, items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_STR_EQ(items[0].source, "apple_photos");
    HU_ASSERT_STR_EQ(items[0].content_type, "photo");
    HU_ASSERT_TRUE(strstr(items[0].content, "Mock photo") != NULL);
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
    HU_ASSERT_STR_EQ(items[1].source, "apple_photos");
}

static void apple_photos_fetch_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_apple_photos_fetch(&alloc, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void apple_photos_fetch_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[1];
    size_t count = 0;
    hu_error_t err = hu_apple_photos_fetch(&alloc, items, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void apple_contacts_fetch_mock_returns_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_apple_contacts_fetch(&alloc, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "Mock Contact") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "[") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "]") != NULL);
}

static void apple_contacts_fetch_null_buffer_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    hu_error_t err = hu_apple_contacts_fetch(&alloc, NULL, 256, &len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void apple_reminders_fetch_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_apple_reminders_fetch(&alloc, items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_STR_EQ(items[0].source, "apple_reminders");
    HU_ASSERT_STR_EQ(items[0].content_type, "reminder");
    HU_ASSERT_TRUE(strstr(items[0].content, "Mock reminder") != NULL);
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
    HU_ASSERT_STR_EQ(items[1].source, "apple_reminders");
}

static void apple_reminders_fetch_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_apple_reminders_fetch(&alloc, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_apple_feeds_tests(void) {
    HU_TEST_SUITE("apple feeds");
    HU_RUN_TEST(apple_photos_fetch_mock_returns_two_items);
    HU_RUN_TEST(apple_photos_fetch_null_items_returns_error);
    HU_RUN_TEST(apple_photos_fetch_insufficient_cap_returns_error);
    HU_RUN_TEST(apple_contacts_fetch_mock_returns_json);
    HU_RUN_TEST(apple_contacts_fetch_null_buffer_returns_error);
    HU_RUN_TEST(apple_reminders_fetch_mock_returns_two_items);
    HU_RUN_TEST(apple_reminders_fetch_null_items_returns_error);
}

#endif /* HU_ENABLE_FEEDS */
