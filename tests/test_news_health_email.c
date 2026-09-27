typedef int hu_test_news_health_email_unused_;

#ifdef HU_ENABLE_FEEDS

#include "human/core/allocator.h"
#include "human/feeds/news.h"
#include "test_framework.h"
#include <string.h>

/* ── RSS (F90) ───────────────────────────────────────────────────────────── */

static void news_fetch_rss_mock_returns_two_articles(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rss_article_t articles[4];
    size_t count = 0;
    hu_error_t err = hu_news_fetch_rss(&alloc, "https://example.com/feed", 22, articles, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(articles[0].title, "Tech News: AI Advances");
    HU_ASSERT_STR_EQ(articles[0].link, "https://example.com/article/1");
    HU_ASSERT_TRUE(strstr(articles[0].description, "Latest in AI research") != NULL);
    HU_ASSERT_TRUE(articles[0].pub_date > 0);
    HU_ASSERT_STR_EQ(articles[1].title, "Tech News: AI Advances");
    HU_ASSERT_STR_EQ(articles[1].link, "https://example.com/article/2");
}

static void news_fetch_rss_null_articles_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_news_fetch_rss(&alloc, "https://x.com/feed", 18, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void news_fetch_rss_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rss_article_t articles[1];
    size_t count = 0;
    hu_error_t err = hu_news_fetch_rss(&alloc, "https://x.com/feed", 18, articles, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_news_health_email_tests(void) {
    HU_TEST_SUITE("news feed (RSS)");
    HU_RUN_TEST(news_fetch_rss_mock_returns_two_articles);
    HU_RUN_TEST(news_fetch_rss_null_articles_returns_error);
    HU_RUN_TEST(news_fetch_rss_insufficient_cap_returns_error);
}

#else

void run_news_health_email_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_FEEDS */
