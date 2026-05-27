/* Tests for src/doctor/ws_consumer.c — T1 pure helpers.
 *
 * Spec: docs/plans/2026-05-27-doctor-ws-consumer/
 *
 * Covers:
 *  - hu_doctor_ws_event_matches_filter — NULL, empty, single, multi,
 *    whitespace-trim, non-match
 *  - hu_doctor_ws_format_event_line — happy path, NULL inputs, deterministic
 *    timestamp via fixed epoch, summary truncation
 *  - hu_doctor_ws_config_default — sane defaults
 *  - hu_doctor_ws_watch — stub returns HU_ERR_NOT_SUPPORTED until T2-T6 ship
 */

#include "human/doctor/ws_consumer.h"
#include "test_framework.h"

#include <string.h>
#include <time.h>

static void test_event_matches_filter_null_matches_all(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", NULL));
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("agent.tool", NULL));
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("anything", NULL));
}

static void test_event_matches_filter_empty_matches_all(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", ""));
}

static void test_event_matches_filter_single_token_match(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", "chat"));
}

static void test_event_matches_filter_single_token_no_match(void) {
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("error", "chat"));
}

static void test_event_matches_filter_csv_match_first(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", "chat,error,health"));
}

static void test_event_matches_filter_csv_match_middle(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("error", "chat,error,health"));
}

static void test_event_matches_filter_csv_match_last(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("health", "chat,error,health"));
}

static void test_event_matches_filter_csv_no_match(void) {
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("cron.job", "chat,error,health"));
}

static void test_event_matches_filter_trims_whitespace(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", " chat , error "));
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("error", " chat , error "));
}

static void test_event_matches_filter_empty_tokens_ignored(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", ",,chat,,"));
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("foo", ",,chat,,"));
}

static void test_event_matches_filter_null_event_returns_false(void) {
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter(NULL, "chat"));
}

static void test_event_matches_filter_exact_match_no_prefix(void) {
    /* "chat" filter must NOT match "chat.send" — no glob in v1. */
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("chat.send", "chat"));
}

static void test_format_event_line_includes_name_and_seq(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 2026-05-27T12:34:56 UTC for deterministic timestamp (localtime will
     * still vary by TZ, but seq + name are deterministic). */
    char *line = hu_doctor_ws_format_event_line(&alloc, "chat", "{}", 42, 1748781296);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_TRUE(strstr(line, "chat") != NULL);
    HU_ASSERT_TRUE(strstr(line, "seq=42") != NULL);
    /* Format is "[HH:MM:SS] ... " — starts with '[' and contains a ']'. */
    HU_ASSERT_TRUE(line[0] == '[');
    HU_ASSERT_TRUE(strchr(line, ']') != NULL);
    alloc.free(alloc.ctx, line, strlen(line) + 1);
}

static void test_format_event_line_null_alloc_returns_null(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_format_event_line(NULL, "chat", "{}", 0, 0) == NULL);
}

static void test_format_event_line_null_name_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_TRUE(hu_doctor_ws_format_event_line(&alloc, NULL, "{}", 0, 0) == NULL);
}

static void test_format_event_line_collapses_payload_whitespace(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *line = hu_doctor_ws_format_event_line(
        &alloc, "agent.tool", "{\n  \"name\": \"ping\",\n  \"args\": {}\n}", 7, 1748781296);
    HU_ASSERT_NOT_NULL(line);
    /* Newlines & tabs must be collapsed to single spaces — the line
     * must NOT contain '\n'. */
    HU_ASSERT_TRUE(strchr(line, '\n') == NULL);
    HU_ASSERT_TRUE(strstr(line, "agent.tool") != NULL);
    HU_ASSERT_TRUE(strstr(line, "seq=7") != NULL);
    alloc.free(alloc.ctx, line, strlen(line) + 1);
}

static void test_format_event_line_truncates_long_payload(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Payload > 80 chars; summary should be truncated with "..." */
    char big_payload[256];
    for (int i = 0; i < 200; i++)
        big_payload[i] = 'x';
    big_payload[200] = '\0';
    char *line = hu_doctor_ws_format_event_line(&alloc, "error", big_payload, 1, 1748781296);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_TRUE(strstr(line, "...") != NULL);
    /* Sanity: shouldn't contain all 200 x's — must have been truncated */
    HU_ASSERT_TRUE(strlen(line) < 200);
    alloc.free(alloc.ctx, line, strlen(line) + 1);
}

static void test_config_default_has_sane_values(void) {
    hu_doctor_ws_config_t c = hu_doctor_ws_config_default();
    HU_ASSERT_TRUE(c.host != NULL);
    HU_ASSERT_TRUE(strcmp(c.host, "127.0.0.1") == 0);
    HU_ASSERT_EQ((int)c.port, 3006);
    HU_ASSERT_TRUE(c.path != NULL);
    HU_ASSERT_TRUE(strcmp(c.path, "/ws") == 0);
    HU_ASSERT_TRUE(c.event_filter == NULL);
    HU_ASSERT_EQ((int)c.max_reconnect_attempts, 3);
    HU_ASSERT_TRUE(c.quiet_stdout == false);
}

static void test_watch_returns_not_supported_until_t2_lands(void) {
    /* T1 ships the stub; T2-T6 will implement the socket loop. Until
     * then, hu_doctor_ws_watch returns HU_ERR_NOT_SUPPORTED so callers
     * can detect the gap explicitly rather than hanging on a half-built
     * loop. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_doctor_ws_config_t c = hu_doctor_ws_config_default();
    HU_ASSERT_EQ(hu_doctor_ws_watch(&alloc, &c), HU_ERR_NOT_SUPPORTED);
}

void run_doctor_ws_consumer_tests(void) {
    HU_TEST_SUITE("doctor-ws-consumer");
    HU_RUN_TEST(test_event_matches_filter_null_matches_all);
    HU_RUN_TEST(test_event_matches_filter_empty_matches_all);
    HU_RUN_TEST(test_event_matches_filter_single_token_match);
    HU_RUN_TEST(test_event_matches_filter_single_token_no_match);
    HU_RUN_TEST(test_event_matches_filter_csv_match_first);
    HU_RUN_TEST(test_event_matches_filter_csv_match_middle);
    HU_RUN_TEST(test_event_matches_filter_csv_match_last);
    HU_RUN_TEST(test_event_matches_filter_csv_no_match);
    HU_RUN_TEST(test_event_matches_filter_trims_whitespace);
    HU_RUN_TEST(test_event_matches_filter_empty_tokens_ignored);
    HU_RUN_TEST(test_event_matches_filter_null_event_returns_false);
    HU_RUN_TEST(test_event_matches_filter_exact_match_no_prefix);
    HU_RUN_TEST(test_format_event_line_includes_name_and_seq);
    HU_RUN_TEST(test_format_event_line_null_alloc_returns_null);
    HU_RUN_TEST(test_format_event_line_null_name_returns_null);
    HU_RUN_TEST(test_format_event_line_collapses_payload_whitespace);
    HU_RUN_TEST(test_format_event_line_truncates_long_payload);
    HU_RUN_TEST(test_config_default_has_sane_values);
    HU_RUN_TEST(test_watch_returns_not_supported_until_t2_lands);
}
