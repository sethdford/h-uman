typedef int hu_test_research_feeds_unused_;

#ifdef HU_ENABLE_FEEDS

#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/feeds/gmail.h"
#include "human/feeds/imessage.h"
#include "human/feeds/twitter.h"
#include "human/feeds/file_ingest.h"
#include "human/feeds/processor.h"
#include "human/feeds/research.h"
#include "test_framework.h"
#include <string.h>

/* ── Gmail feed (F94) ─────────────────────────────────────────────────── */

static void gmail_feed_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_gmail_feed_fetch(&alloc,
        "test-id", 7, "test-secret", 11, "test-token", 10,
        items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(items[0].source, "gmail");
    HU_ASSERT_STR_EQ(items[0].content_type, "email");
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(strstr(items[0].content, "AI") != NULL);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
}

static void gmail_feed_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_gmail_feed_fetch(&alloc,
        "id", 2, "secret", 6, "token", 5,
        NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void gmail_feed_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[1];
    size_t count = 0;
    hu_error_t err = hu_gmail_feed_fetch(&alloc,
        "id", 2, "secret", 6, "token", 5,
        items, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── iMessage feed (F95) ─────────────────────────────────────────────── */

static void imessage_feed_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_imessage_feed_fetch(&alloc, 0, items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(items[0].source, "imessage");
    HU_ASSERT_STR_EQ(items[0].content_type, "message");
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
    HU_ASSERT_TRUE(items[0].contact_id[0] != '\0');
}

static void imessage_feed_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_imessage_feed_fetch(&alloc, 0, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void imessage_feed_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[1];
    size_t count = 0;
    hu_error_t err = hu_imessage_feed_fetch(&alloc, 0, items, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Twitter feed (F96) ──────────────────────────────────────────────── */

static void twitter_feed_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_twitter_feed_fetch(&alloc,
        "test-bearer", 11, items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(items[0].source, "twitter");
    HU_ASSERT_STR_EQ(items[0].content_type, "tweet");
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].url[0] != '\0');
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
}

static void twitter_feed_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_twitter_feed_fetch(&alloc,
        "test-bearer", 11, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void twitter_feed_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[1];
    size_t count = 0;
    hu_error_t err = hu_twitter_feed_fetch(&alloc,
        "test-bearer", 11, items, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── File ingest (F98) ───────────────────────────────────────────────── */

static void file_ingest_mock_returns_two_items(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[4];
    size_t count = 0;
    hu_error_t err = hu_file_ingest_fetch(&alloc, items, 4, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_TRUE(items[0].content_len > 0);
    HU_ASSERT_TRUE(items[0].ingested_at > 0);
    HU_ASSERT_TRUE(items[0].source[0] != '\0');
}

static void file_ingest_null_items_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_file_ingest_fetch(&alloc, NULL, 4, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void file_ingest_insufficient_cap_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feed_ingest_item_t items[1];
    size_t count = 0;
    hu_error_t err = hu_file_ingest_fetch(&alloc, items, 1, &count);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Feed type strings for new types ──────────────────────────────────── */

static void feed_type_str_new_types_valid(void) {
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_GMAIL), "gmail");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_IMESSAGE), "imessage");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_TWITTER), "twitter");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_TIKTOK), "tiktok");
    HU_ASSERT_STR_EQ(hu_feed_type_str(HU_FEED_FILE_INGEST), "file_ingest");
}

/* ── Research agent prompt ────────────────────────────────────────────── */

static void research_prompt_not_null(void) {
    const char *prompt = hu_research_agent_prompt();
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strlen(prompt) > 100);
    HU_ASSERT_NOT_NULL(strstr(prompt, "Research Agent"));
    HU_ASSERT_NOT_NULL(strstr(prompt, "feed items"));
}

static void research_cron_expression_valid(void) {
    const char *cron = hu_research_cron_expression();
    HU_ASSERT_NOT_NULL(cron);
    HU_ASSERT_STR_EQ(cron, "0 6 * * *");
}

static void research_build_prompt_with_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *summary = "RSS: New transformer architecture. Twitter: Claude 4 released.";
    size_t summary_len = strlen(summary);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_research_build_prompt(&alloc, summary, summary_len, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "Research Agent"));
    HU_ASSERT_NOT_NULL(strstr(out, "transformer"));
    HU_ASSERT_NOT_NULL(strstr(out, "Claude 4"));
    hu_str_free(&alloc, out);
}

static void research_build_prompt_null_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_research_build_prompt(&alloc, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "No feed items today"));
    hu_str_free(&alloc, out);
}

static void research_build_prompt_null_args_returns_error(void) {
    hu_error_t err = hu_research_build_prompt(NULL, NULL, 0, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Self-improvement agent ──────────────────────────────────────────── */

static void self_improve_prompt_not_null(void) {
    const char *prompt = hu_research_self_improve_prompt();
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strlen(prompt) > 100);
    HU_ASSERT_NOT_NULL(strstr(prompt, "Self-Improvement Agent"));
    HU_ASSERT_NOT_NULL(strstr(prompt, "NEVER"));
    HU_ASSERT_NOT_NULL(strstr(prompt, "branch"));
}

static void build_action_prompt_with_finding(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *finding = "New embedding model reduces memory 40%";
    const char *action = "Integrate into src/memory/embeddings.c";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_research_build_action_prompt(&alloc,
        finding, strlen(finding), action, strlen(action),
        &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "Self-Improvement Agent"));
    HU_ASSERT_NOT_NULL(strstr(out, "embedding model"));
    HU_ASSERT_NOT_NULL(strstr(out, "embeddings.c"));
    HU_ASSERT_NOT_NULL(strstr(out, "Implement this improvement"));
    hu_str_free(&alloc, out);
}

static void build_action_prompt_null_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *finding = "New provider available";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_research_build_action_prompt(&alloc,
        finding, strlen(finding), NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, "Determine best action"));
    hu_str_free(&alloc, out);
}

static void build_action_prompt_null_args_returns_error(void) {
    hu_error_t err = hu_research_build_action_prompt(NULL, NULL, 0, NULL, 0, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Feed should_poll works for new types ──────────────────────────────── */

static void feeds_should_poll_gmail_enabled(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_GMAIL] = true;
    config.poll_interval_minutes[HU_FEED_GMAIL] = 360;
    uint64_t last = 1000000;
    uint64_t now = last + (360ULL * 60 * 1000);
    HU_ASSERT_TRUE(hu_feeds_should_poll(HU_FEED_GMAIL, &config, last, now));
}

static void feeds_should_poll_imessage_enabled(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_IMESSAGE] = true;
    config.poll_interval_minutes[HU_FEED_IMESSAGE] = 60;
    uint64_t last = 1000000;
    uint64_t now = last + (60ULL * 60 * 1000);
    HU_ASSERT_TRUE(hu_feeds_should_poll(HU_FEED_IMESSAGE, &config, last, now));
}

static void feeds_should_poll_twitter_enabled(void) {
    hu_feed_config_t config = {0};
    config.enabled[HU_FEED_TWITTER] = true;
    config.poll_interval_minutes[HU_FEED_TWITTER] = 120;
    uint64_t last = 1000000;
    uint64_t now = last + (120ULL * 60 * 1000);
    HU_ASSERT_TRUE(hu_feeds_should_poll(HU_FEED_TWITTER, &config, last, now));
}

void run_research_feeds_tests(void) {
    HU_TEST_SUITE("research_feeds");

    /* Gmail feed */
    HU_RUN_TEST(gmail_feed_mock_returns_two_items);
    HU_RUN_TEST(gmail_feed_null_items_returns_error);
    HU_RUN_TEST(gmail_feed_insufficient_cap_returns_error);

    /* iMessage feed */
    HU_RUN_TEST(imessage_feed_mock_returns_two_items);
    HU_RUN_TEST(imessage_feed_null_items_returns_error);
    HU_RUN_TEST(imessage_feed_insufficient_cap_returns_error);

    /* Twitter feed */
    HU_RUN_TEST(twitter_feed_mock_returns_two_items);
    HU_RUN_TEST(twitter_feed_null_items_returns_error);
    HU_RUN_TEST(twitter_feed_insufficient_cap_returns_error);

    /* File ingest */
    HU_RUN_TEST(file_ingest_mock_returns_two_items);
    HU_RUN_TEST(file_ingest_null_items_returns_error);
    HU_RUN_TEST(file_ingest_insufficient_cap_returns_error);

    /* Feed type strings */
    HU_RUN_TEST(feed_type_str_new_types_valid);

    /* Research agent */
    HU_RUN_TEST(research_prompt_not_null);
    HU_RUN_TEST(research_cron_expression_valid);
    HU_RUN_TEST(research_build_prompt_with_summary);
    HU_RUN_TEST(research_build_prompt_null_summary);
    HU_RUN_TEST(research_build_prompt_null_args_returns_error);

    /* Self-improvement agent */
    HU_RUN_TEST(self_improve_prompt_not_null);
    HU_RUN_TEST(build_action_prompt_with_finding);
    HU_RUN_TEST(build_action_prompt_null_action);
    HU_RUN_TEST(build_action_prompt_null_args_returns_error);

    /* Feed polling for new types */
    HU_RUN_TEST(feeds_should_poll_gmail_enabled);
    HU_RUN_TEST(feeds_should_poll_imessage_enabled);
    HU_RUN_TEST(feeds_should_poll_twitter_enabled);
}

#else

void run_research_feeds_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_FEEDS */
