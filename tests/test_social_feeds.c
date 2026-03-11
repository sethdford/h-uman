#ifdef HU_ENABLE_SOCIAL

#include "human/core/allocator.h"
#include "human/feeds/social.h"
#include "test_framework.h"
#include <string.h>

static void social_fetch_facebook_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_social_fetch_facebook(&alloc, NULL, "test-token", 10,
        items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_STR_EQ(items[0].source, "facebook");
    HU_ASSERT_STR_EQ(items[0].content_type, "post");
    HU_ASSERT_TRUE(strstr(items[0].content, "Mock Facebook") != NULL);
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
    HU_ASSERT_STR_EQ(items[1].source, "facebook");
    HU_ASSERT_STR_EQ(items[1].content_type, "post");
}

static void social_fetch_facebook_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_social_fetch_facebook(&alloc, NULL, "token", 4,
        NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void social_fetch_facebook_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[1];
    size_t count = 0;
    hu_error_t err = hu_social_fetch_facebook(&alloc, NULL, "token", 4,
        items, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void social_fetch_instagram_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_social_fetch_instagram(&alloc, NULL, "test-token", 10,
        items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_STR_EQ(items[0].source, "instagram");
    HU_ASSERT_STR_EQ(items[0].content_type, "media");
    HU_ASSERT_TRUE(strstr(items[0].content, "Mock Instagram") != NULL);
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
    HU_ASSERT_STR_EQ(items[1].source, "instagram");
    HU_ASSERT_STR_EQ(items[1].content_type, "media");
}

static void social_fetch_instagram_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_social_fetch_instagram(&alloc, NULL, "token", 4,
        NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_social_feeds_tests(void) {
    HU_TEST_SUITE("social feeds");
    HU_RUN_TEST(social_fetch_facebook_mock_returns_two_items);
    HU_RUN_TEST(social_fetch_facebook_null_items_returns_error);
    HU_RUN_TEST(social_fetch_facebook_insufficient_cap_returns_error);
    HU_RUN_TEST(social_fetch_instagram_mock_returns_two_items);
    HU_RUN_TEST(social_fetch_instagram_null_items_returns_error);
}

#endif /* HU_ENABLE_SOCIAL */
