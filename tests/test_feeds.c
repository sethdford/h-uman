#include "human/core/allocator.h"
#include "human/feeds/processor.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void feeds_create_table_sql_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_feeds_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "CREATE TABLE"));
    HU_ASSERT_NOT_NULL(strstr(buf, "feed_items"));
    HU_ASSERT_NOT_NULL(strstr(buf, "type"));
    HU_ASSERT_NOT_NULL(strstr(buf, "source"));
    HU_ASSERT_NOT_NULL(strstr(buf, "content"));
    HU_ASSERT_NOT_NULL(strstr(buf, "relevance"));
    HU_ASSERT_NOT_NULL(strstr(buf, "processed"));
}

static void feeds_insert_sql_valid(void) {
    hu_feed_item_t item = {
        .id = 0,
        .type = HU_FEED_SOCIAL_INSTAGRAM,
        .source = "instagram:@friend",
        .source_len = 17,
        .content = "posted about hiking",
        .content_len = 19,
        .topic = "outdoors",
        .topic_len = 8,
        .relevance = 0.85,
        .fetched_at = 1700000000000ULL,
        .processed = false,
    };
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_feeds_insert_sql(&item, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "INSERT INTO"));
    HU_ASSERT_NOT_NULL(strstr(buf, "social_instagram"));
    HU_ASSERT_NOT_NULL(strstr(buf, "instagram:@friend"));
    HU_ASSERT_NOT_NULL(strstr(buf, "posted about hiking"));
    HU_ASSERT_NOT_NULL(strstr(buf, "outdoors"));
}

static void feeds_query_unprocessed_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err =
        hu_feeds_query_unprocessed_sql(HU_FEED_NEWS_RSS, 10, buf,
                                        sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "SELECT"));
    HU_ASSERT_NOT_NULL(strstr(buf, "news_rss"));
    HU_ASSERT_NOT_NULL(strstr(buf, "processed = 0"));
    HU_ASSERT_NOT_NULL(strstr(buf, "LIMIT 10"));
}

static void feeds_mark_processed_sql_valid(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_feeds_mark_processed_sql(42, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "UPDATE"));
    HU_ASSERT_NOT_NULL(strstr(buf, "processed = 1"));
    HU_ASSERT_NOT_NULL(strstr(buf, "id = 42"));
}

static void feeds_query_by_topic_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    const char *topic = "hiking";
    hu_error_t err = hu_feeds_query_by_topic_sql(
        topic, 6, 5, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "SELECT"));
    HU_ASSERT_NOT_NULL(strstr(buf, "topic = 'hiking'"));
    HU_ASSERT_NOT_NULL(strstr(buf, "LIMIT 5"));
}

static void feeds_should_poll_enabled_interval_elapsed(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_MUSIC] = true;
    config.poll_interval_minutes[HU_FEED_MUSIC] = 60;
    uint64_t last = 1000000;
    uint64_t now = 1000000 + (60ULL * 60 * 1000);
    HU_ASSERT_TRUE(
        hu_feeds_should_poll(HU_FEED_MUSIC, &config, last, now));
}

static void feeds_should_poll_disabled_returns_false(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_EMAIL] = false;
    config.poll_interval_minutes[HU_FEED_EMAIL] = 30;
    uint64_t last = 1000000;
    uint64_t now = 1000000 + (60ULL * 60 * 1000);
    HU_ASSERT_FALSE(
        hu_feeds_should_poll(HU_FEED_EMAIL, &config, last, now));
}

static void feeds_should_poll_interval_not_elapsed_returns_false(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_NEWS_RSS] = true;
    config.poll_interval_minutes[HU_FEED_NEWS_RSS] = 60;
    uint64_t last = 1000000;
    uint64_t now = 1000000 + (30ULL * 60 * 1000);
    HU_ASSERT_FALSE(
        hu_feeds_should_poll(HU_FEED_NEWS_RSS, &config, last, now));
}

static void feeds_should_poll_interval_zero_returns_true_when_enabled(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_APPLE_REMINDERS] = true;
    config.poll_interval_minutes[HU_FEED_APPLE_REMINDERS] = 0;
    uint64_t last = 1000000;
    uint64_t now = 1000000;
    HU_ASSERT_TRUE(hu_feeds_should_poll(HU_FEED_APPLE_REMINDERS, &config,
                                         last, now));
}

static void feeds_feed_type_str_all_types(void) {
    HU_ASSERT_NOT_NULL(hu_feed_type_str(HU_FEED_SOCIAL_FACEBOOK));
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_SOCIAL_FACEBOOK),
                     "social_facebook");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_SOCIAL_INSTAGRAM),
                     "social_instagram");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_MUSIC), "music");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_NEWS_RSS), "news_rss");
}

static void feeds_score_relevance_high_overlap(void) {
    size_t content_len = strlen("hiking mountains trail");
    size_t interest_len = strlen("hiking trail");
    double score = hu_feeds_score_relevance("hiking mountains trail",
                                          content_len, "hiking trail",
                                          interest_len);
    HU_ASSERT_TRUE(score >= 0.99);
}

static void feeds_score_relevance_no_overlap(void) {
    size_t content_len = strlen("cooking pizza");
    size_t interest_len = strlen("hiking trail");
    double score = hu_feeds_score_relevance("cooking pizza", content_len,
                                           "hiking trail", interest_len);
    HU_ASSERT_FLOAT_EQ(score, 0.0, 0.001);
}

static void feeds_score_relevance_partial(void) {
    size_t content_len = strlen("friend posted about hiking");
    size_t interest_len = strlen("hiking outdoors");
    double score = hu_feeds_score_relevance("friend posted about hiking",
                                           content_len, "hiking outdoors",
                                           interest_len);
    HU_ASSERT_TRUE(score > 0.0 && score < 1.0);
}

static void feeds_score_relevance_null_returns_zero(void) {
    double score = hu_feeds_score_relevance(NULL, 0, "hiking", 5);
    HU_ASSERT_FLOAT_EQ(score, 0.0, 0.001);
}

static void feeds_build_prompt_empty_returns_none(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_feeds_build_prompt(&alloc, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "[EXTERNAL AWARENESS]"));
    HU_ASSERT_NOT_NULL(strstr(out, "(none)"));
    hu_str_free(&alloc, out);
}

static void feeds_build_prompt_single_item(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_item_t item = {
        .type = HU_FEED_SOCIAL_INSTAGRAM,
        .content = "friend posted about hiking",
        .content_len = 26,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_feeds_build_prompt(&alloc, &item, 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "[EXTERNAL AWARENESS]"));
    HU_ASSERT_NOT_NULL(strstr(out, "Social"));
    HU_ASSERT_NOT_NULL(strstr(out, "friend posted about hiking"));
    hu_str_free(&alloc, out);
}

static void feeds_build_prompt_multiple_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_item_t items[3] = {
        {
            .type = HU_FEED_SOCIAL_INSTAGRAM,
            .content = "friend posted about hiking",
            .content_len = 26,
        },
        {
            .type = HU_FEED_NEWS_RSS,
            .content = "new AI chip announced",
            .content_len = 21,
        },
        {
            .type = HU_FEED_MUSIC,
            .content = "listening to artist",
            .content_len = 19,
        },
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_feeds_build_prompt(&alloc, items, 3, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "[EXTERNAL AWARENESS]"));
    HU_ASSERT_NOT_NULL(strstr(out, "Social"));
    HU_ASSERT_NOT_NULL(strstr(out, "News"));
    HU_ASSERT_NOT_NULL(strstr(out, "Music"));
    HU_ASSERT_NOT_NULL(strstr(out, "friend posted about hiking"));
    HU_ASSERT_NOT_NULL(strstr(out, "new AI chip announced"));
    HU_ASSERT_NOT_NULL(strstr(out, "listening to artist"));
    hu_str_free(&alloc, out);
}

static void feeds_feed_item_deinit_frees_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_item_t item;
    memset(&item, 0, sizeof(item));
    item.source = hu_strndup(&alloc, "instagram:@friend", 17);
    item.source_len = 17;
    item.content = hu_strndup(&alloc, "test content", 11);
    item.content_len = 11;
    item.topic = hu_strndup(&alloc, "outdoors", 8);
    item.topic_len = 8;
    HU_ASSERT_NOT_NULL(item.source);
    HU_ASSERT_NOT_NULL(item.content);
    HU_ASSERT_NOT_NULL(item.topic);
    hu_feed_item_deinit(&alloc, &item);
    HU_ASSERT_NULL(item.source);
    HU_ASSERT_NULL(item.content);
    HU_ASSERT_NULL(item.topic);
    HU_ASSERT_EQ(item.source_len, 0);
    HU_ASSERT_EQ(item.content_len, 0);
    HU_ASSERT_EQ(item.topic_len, 0);
}

void run_feeds_tests(void) {
    HU_TEST_SUITE("feeds");
    HU_RUN_TEST(feeds_create_table_sql_valid);
    HU_RUN_TEST(feeds_insert_sql_valid);
    HU_RUN_TEST(feeds_query_unprocessed_sql_valid);
    HU_RUN_TEST(feeds_mark_processed_sql_valid);
    HU_RUN_TEST(feeds_query_by_topic_sql_valid);
    HU_RUN_TEST(feeds_should_poll_enabled_interval_elapsed);
    HU_RUN_TEST(feeds_should_poll_disabled_returns_false);
    HU_RUN_TEST(feeds_should_poll_interval_not_elapsed_returns_false);
    HU_RUN_TEST(feeds_should_poll_interval_zero_returns_true_when_enabled);
    HU_RUN_TEST(feeds_feed_type_str_all_types);
    HU_RUN_TEST(feeds_score_relevance_high_overlap);
    HU_RUN_TEST(feeds_score_relevance_no_overlap);
    HU_RUN_TEST(feeds_score_relevance_partial);
    HU_RUN_TEST(feeds_score_relevance_null_returns_zero);
    HU_RUN_TEST(feeds_build_prompt_empty_returns_none);
    HU_RUN_TEST(feeds_build_prompt_single_item);
    HU_RUN_TEST(feeds_build_prompt_multiple_items);
    HU_RUN_TEST(feeds_feed_item_deinit_frees_memory);
}
