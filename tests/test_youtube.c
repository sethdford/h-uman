#include "human/core/allocator.h"
#include "human/youtube.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

static const char YT_OK[] =
    "{\"items\":[{\"id\":{\"videoId\":\"dQw4w9WgXcQ\"},"
    "\"snippet\":{\"title\":\"Latte Art Basics\",\"channelTitle\":\"CoffeeCo\"}}]}";
static const char YT_EMPTY[] = "{\"items\":[]}";

static void test_youtube_parse_builds_canonical_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_youtube_result_t r = {0};
    HU_ASSERT_EQ((int)hu_youtube_parse_search_response(&alloc, YT_OK, sizeof(YT_OK) - 1, &r),
                 (int)HU_OK);
    HU_ASSERT_TRUE(r.video_id && strcmp(r.video_id, "dQw4w9WgXcQ") == 0);
    HU_ASSERT_TRUE(r.watch_url &&
                   strcmp(r.watch_url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ") == 0);
    hu_youtube_result_free(&alloc, &r);
}

static void test_youtube_parse_empty_items_errors(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_youtube_result_t r = {0};
    HU_ASSERT_TRUE(hu_youtube_parse_search_response(&alloc, YT_EMPTY, sizeof(YT_EMPTY) - 1, &r) !=
                   HU_OK);
    hu_youtube_result_free(&alloc, &r);
}

static void test_youtube_parse_null_args_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_youtube_result_t r = {0};
    HU_ASSERT_TRUE(hu_youtube_parse_search_response(&alloc, NULL, 0, &r) != HU_OK);
}

/* Phase 5.3 (spec 2026-09-28): "send a funny short about X" searches Shorts
 * only, and the link opens as a Short. */
static void test_youtube_search_url_filters_shorts(void) {
    char url[1024];
    HU_ASSERT_TRUE(hu_youtube_search_url(url, sizeof(url), "test-key", "cat fail", 8, true) > 0);
    HU_ASSERT_STR_CONTAINS(url, "videoDuration=short");
    HU_ASSERT_STR_CONTAINS(url, "q=cat");
    HU_ASSERT_STR_CONTAINS(url, "key=test-key");
    HU_ASSERT_TRUE(hu_youtube_search_url(url, sizeof(url), "test-key", "cat fail", 8, false) > 0);
    HU_ASSERT_STR_NOT_CONTAINS(url, "videoDuration");
    HU_ASSERT_EQ(hu_youtube_search_url(url, 20, "test-key", "cat fail", 8, true), 0u);
}

static void test_youtube_short_link_opens_as_a_short(void) {
    char url[128];
    HU_ASSERT_TRUE(hu_youtube_share_url(url, sizeof(url), "abc123", true) > 0);
    HU_ASSERT_STR_EQ(url, "https://www.youtube.com/shorts/abc123");
    HU_ASSERT_TRUE(hu_youtube_share_url(url, sizeof(url), "abc123", false) > 0);
    HU_ASSERT_STR_EQ(url, "https://www.youtube.com/watch?v=abc123");
}

void run_youtube_tests(void) {
    HU_TEST_SUITE("youtube");
    HU_RUN_TEST(test_youtube_parse_builds_canonical_url);
    HU_RUN_TEST(test_youtube_search_url_filters_shorts);
    HU_RUN_TEST(test_youtube_short_link_opens_as_a_short);
    HU_RUN_TEST(test_youtube_parse_empty_items_errors);
    HU_RUN_TEST(test_youtube_parse_null_args_rejected);
}
