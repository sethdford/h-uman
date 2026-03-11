typedef int hu_test_news_health_email_unused_;

#ifdef HU_ENABLE_FEEDS

#include "human/core/allocator.h"
#include "human/feeds/apple.h"
#include "human/feeds/email.h"
#include "human/feeds/news.h"
#include "test_framework.h"
#include <string.h>

/* ── RSS (F90) ───────────────────────────────────────────────────────────── */

static void news_fetch_rss_mock_returns_two_articles(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rss_article_t articles[4];
    size_t count = 0;
    hu_error_t err = hu_news_fetch_rss(&alloc, "https://example.com/feed", 22,
        articles, 4, &count);
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
    hu_error_t err = hu_news_fetch_rss(&alloc, "https://x.com/feed", 18,
        NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void news_fetch_rss_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rss_article_t articles[1];
    size_t count = 0;
    hu_error_t err = hu_news_fetch_rss(&alloc, "https://x.com/feed", 18,
        articles, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Apple Health (F91) ─────────────────────────────────────────────────── */

static void apple_health_parse_export_fixture_verifies_steps(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_health_summary_t out = {0};
    hu_error_t err = hu_apple_health_parse_export(&alloc, "<?xml", 5, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.steps_today, 8000);
    HU_ASSERT_TRUE(out.avg_heart_rate >= 68.0 && out.avg_heart_rate <= 72.0);
    HU_ASSERT_TRUE(out.sleep_hours >= 7.9 && out.sleep_hours <= 8.1);
    HU_ASSERT_TRUE(out.export_date > 0);
}

static void apple_health_parse_export_null_out_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_apple_health_parse_export(&alloc, "<x/>", 4, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Email (F92) ──────────────────────────────────────────────────────────── */

static void email_fetch_unread_mock_returns_two_headers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_email_header_t headers[4];
    size_t count = 0;
    hu_error_t err = hu_email_fetch_unread(&alloc, headers, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(headers[0].subject, "Meeting Tomorrow");
    HU_ASSERT_STR_EQ(headers[0].from, "alice@example.com");
    HU_ASSERT_TRUE(headers[0].date > 0);
    HU_ASSERT_STR_EQ(headers[1].subject, "Project Update");
    HU_ASSERT_STR_EQ(headers[1].from, "bob@example.com");
}

static void email_fetch_unread_null_headers_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_email_fetch_unread(&alloc, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void email_fetch_unread_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_email_header_t headers[1];
    size_t count = 0;
    hu_error_t err = hu_email_fetch_unread(&alloc, headers, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_news_health_email_tests(void) {
    HU_TEST_SUITE("news_health_email");
    HU_RUN_TEST(news_fetch_rss_mock_returns_two_articles);
    HU_RUN_TEST(news_fetch_rss_null_articles_returns_error);
    HU_RUN_TEST(news_fetch_rss_insufficient_cap_returns_error);
    HU_RUN_TEST(apple_health_parse_export_fixture_verifies_steps);
    HU_RUN_TEST(apple_health_parse_export_null_out_returns_error);
    HU_RUN_TEST(email_fetch_unread_mock_returns_two_headers);
    HU_RUN_TEST(email_fetch_unread_null_headers_returns_error);
    HU_RUN_TEST(email_fetch_unread_insufficient_cap_returns_error);
}

#else

void run_news_health_email_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_FEEDS */
