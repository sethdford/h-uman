typedef int hu_test_music_feeds_unused_;

#ifdef HU_ENABLE_FEEDS

#include "human/core/allocator.h"
#include "human/feeds/music.h"
#include "test_framework.h"
#include <string.h>

static void spotify_fetch_mock_returns_correct_source(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_spotify_fetch_recent(&alloc, NULL, "token", 5,
        items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_STR_EQ(items[0].source, "spotify");
    HU_ASSERT_STR_EQ(items[1].source, "spotify");
    HU_ASSERT_STR_EQ(items[2].source, "spotify");
}

static void spotify_fetch_item_count_matches_three(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_spotify_fetch_recent(&alloc, NULL, "token", 5,
        items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3u);
}

static void spotify_fetch_content_contains_artist_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_spotify_fetch_recent(&alloc, NULL, "token", 5,
        items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1u);
    HU_ASSERT_TRUE(strstr(items[0].content, "Queen") != NULL);
    HU_ASSERT_TRUE(strstr(items[1].content, "The Weeknd") != NULL);
    HU_ASSERT_TRUE(strstr(items[2].content, "Dua Lipa") != NULL);
}

void run_music_feeds_tests(void) {
    HU_TEST_SUITE("music feeds");
    HU_RUN_TEST(spotify_fetch_mock_returns_correct_source);
    HU_RUN_TEST(spotify_fetch_item_count_matches_three);
    HU_RUN_TEST(spotify_fetch_content_contains_artist_name);
}

#endif /* HU_ENABLE_FEEDS */
