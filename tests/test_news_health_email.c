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

/* Feed backoff (2026-09-30): config polls RSS every 30 min and hnrss.org
 * answered 429/502 107 times in a week. A refused URL is skipped for 2h,
 * doubling to 24h, and cleared by its next 200. */
static void news_backoff_secs_grows_on_refusal_only(void) {
    HU_ASSERT_EQ(hu_news_backoff_secs(429, 0), 7200u);
    HU_ASSERT_EQ(hu_news_backoff_secs(502, 7200), 14400u);
    HU_ASSERT_EQ(hu_news_backoff_secs(503, 86400), 86400u); /* capped */
    HU_ASSERT_EQ(hu_news_backoff_secs(200, 7200), 0u);
    HU_ASSERT_EQ(hu_news_backoff_secs(404, 0), 0u); /* a dead URL is not rate-limiting */
}

static void news_backoff_skips_a_refused_url_until_it_expires(void) {
    const char *hn = "https://hnrss.org/newest?q=AI";
    const char *ok = "https://simonwillison.net/atom/everything/";
    uint64_t t0 = 1790000000000ULL;
    hu_news_backoff_record(hn, 429, t0);
    HU_ASSERT_TRUE(hu_news_backoff_active(hn, t0 + 60ULL * 1000));
    HU_ASSERT_FALSE(hu_news_backoff_active(ok, t0 + 60ULL * 1000));
    HU_ASSERT_FALSE(hu_news_backoff_active(hn, t0 + 7201ULL * 1000));
    hu_news_backoff_record(hn, 429, t0 + 7201ULL * 1000); /* refused again: 4h */
    HU_ASSERT_TRUE(hu_news_backoff_active(hn, t0 + (7201ULL + 10000) * 1000));
    hu_news_backoff_record(hn, 200, t0 + (7201ULL + 20000) * 1000); /* recovered */
    HU_ASSERT_FALSE(hu_news_backoff_active(hn, t0 + (7201ULL + 20001) * 1000));
}

void run_news_health_email_tests(void) {
    HU_TEST_SUITE("news feed (RSS)");
    HU_RUN_TEST(news_backoff_secs_grows_on_refusal_only);
    HU_RUN_TEST(news_backoff_skips_a_refused_url_until_it_expires);
    HU_RUN_TEST(news_fetch_rss_mock_returns_two_articles);
    HU_RUN_TEST(news_fetch_rss_null_articles_returns_error);
    HU_RUN_TEST(news_fetch_rss_insufficient_cap_returns_error);
}

#else

void run_news_health_email_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_FEEDS */
