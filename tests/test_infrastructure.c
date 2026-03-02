#include "test_framework.h"
#include "seaclaw/version.h"
#include "seaclaw/heartbeat.h"
#include "seaclaw/cost.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>

static void test_version_string(void) {
    const char *v = sc_version_string();
    SC_ASSERT_NOT_NULL(v);
    SC_ASSERT(strlen(v) > 0);
}

static void test_heartbeat_init_clamps_interval(void) {
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, true, 2, "/tmp");
    SC_ASSERT_EQ(e.interval_minutes, 5u);
}

static void test_heartbeat_init_preserves_valid_interval(void) {
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, true, 30, "/tmp");
    SC_ASSERT_EQ(e.interval_minutes, 30u);
}

static void test_heartbeat_is_content_empty(void) {
    SC_ASSERT_TRUE(sc_heartbeat_is_empty_content(""));
    SC_ASSERT_TRUE(sc_heartbeat_is_empty_content("# HEARTBEAT\n\n# comment"));
    SC_ASSERT_FALSE(sc_heartbeat_is_empty_content("Check status"));
}

static void test_heartbeat_parse_tasks(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *content = "# Tasks\n\n- Check email\n- Review calendar\nNot a task\n- Third task";
    char **tasks = NULL;
    size_t count = 0;
    sc_error_t err = sc_heartbeat_parse_tasks(&alloc, content, &tasks, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_EQ(count, 3u);
    SC_ASSERT_STR_EQ(tasks[0], "Check email");
    SC_ASSERT_STR_EQ(tasks[1], "Review calendar");
    SC_ASSERT_STR_EQ(tasks[2], "Third task");
    sc_heartbeat_free_tasks(&alloc, tasks, count);
}

static void test_heartbeat_parse_tasks_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char **tasks = NULL;
    size_t count = 0;
    sc_error_t err = sc_heartbeat_parse_tasks(&alloc, "", &tasks, &count);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_EQ(count, 0u);
    SC_ASSERT_NULL(tasks);
}

static void test_token_usage_init(void) {
    sc_cost_entry_t u;
    sc_token_usage_init(&u, "test/model", 1000, 500, 3.0, 15.0);
    SC_ASSERT_EQ(u.input_tokens, 1000ull);
    SC_ASSERT_EQ(u.output_tokens, 500ull);
    SC_ASSERT_EQ(u.total_tokens, 1500ull);
    SC_ASSERT(u.cost_usd > 0.009 && u.cost_usd < 0.012);
}

static void test_cost_tracker_init_deinit(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_error_t err = sc_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    SC_ASSERT_EQ(err, SC_OK);
    sc_cost_tracker_deinit(&t);
}

static void test_cost_tracker_record_and_summary(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_error_t err = sc_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    SC_ASSERT_EQ(err, SC_OK);

    sc_cost_entry_t u;
    sc_token_usage_init(&u, "model", 1000, 500, 1.0, 2.0);
    err = sc_cost_record_usage(&t, &u);
    SC_ASSERT_EQ(err, SC_OK);

    sc_cost_summary_t summary;
    sc_cost_get_summary(&t, 0, &summary);
    SC_ASSERT_EQ(summary.request_count, 1u);
    SC_ASSERT(summary.session_cost_usd > 0.0);
    SC_ASSERT(summary.total_tokens == 1500);

    sc_cost_tracker_deinit(&t);
}

static void test_cost_budget_disabled_allowed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_error_t err = sc_cost_tracker_init(&t, &alloc, "/tmp", false, 10.0, 100.0, 80);
    SC_ASSERT_EQ(err, SC_OK);
    sc_budget_info_t info;
    sc_budget_check_t check = sc_cost_check_budget(&t, 1000.0, &info);
    SC_ASSERT(check == SC_BUDGET_ALLOWED);
    sc_cost_tracker_deinit(&t);
}

static void test_heartbeat_parse_tasks_asterisk_bullet(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *content = "# Tasks\n\n- Task one\n* Task two\n- Task three";
    char **tasks = NULL;
    size_t count = 0;
    sc_error_t err = sc_heartbeat_parse_tasks(&alloc, content, &tasks, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_STR_EQ(tasks[0], "Task one");
    SC_ASSERT_STR_EQ(tasks[1], "Task three");
    if (tasks) sc_heartbeat_free_tasks(&alloc, tasks, count);
}

static void test_heartbeat_is_empty_headers_only(void) {
    SC_ASSERT_TRUE(sc_heartbeat_is_empty_content("# Header\n## Sub\n\n# Another"));
}

static void test_heartbeat_is_empty_blank_lines(void) {
    SC_ASSERT_TRUE(sc_heartbeat_is_empty_content("\n\n\n"));
}

static void test_heartbeat_parse_tasks_skips_empty_bullets(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *content = "- Real task\n- \n- Another real";
    char **tasks = NULL;
    size_t count = 0;
    sc_error_t err = sc_heartbeat_parse_tasks(&alloc, content, &tasks, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 2u);
    if (tasks) sc_heartbeat_free_tasks(&alloc, tasks, count);
}

static void test_heartbeat_engine_init_clamps_to_five(void) {
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, true, 0, "/tmp");
    SC_ASSERT_EQ(e.interval_minutes, 5u);
}

static void test_heartbeat_engine_init_max_interval(void) {
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, true, 1440, "/tmp");
    SC_ASSERT_EQ(e.interval_minutes, 1440u);
}

static void test_heartbeat_engine_disabled(void) {
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, false, 30, "/tmp");
    SC_ASSERT_FALSE(e.enabled);
}

static void test_cost_session_total_single(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    sc_cost_entry_t u;
    sc_token_usage_init(&u, "m", 1000, 500, 1.0, 2.0);
    sc_cost_record_usage(&t, &u);
    double total = sc_cost_session_total(&t);
    SC_ASSERT(total > 0.0);
    sc_cost_tracker_deinit(&t);
}

static void test_cost_session_tokens_accumulation(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    sc_cost_entry_t u1, u2;
    sc_token_usage_init(&u1, "m", 100, 50, 0, 0);
    sc_token_usage_init(&u2, "m", 200, 100, 0, 0);
    sc_cost_record_usage(&t, &u1);
    sc_cost_record_usage(&t, &u2);
    SC_ASSERT_EQ(sc_cost_session_tokens(&t), 450u);
    sc_cost_tracker_deinit(&t);
}

static void test_cost_request_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    SC_ASSERT_EQ(sc_cost_request_count(&t), 0u);
    sc_cost_entry_t u;
    sc_token_usage_init(&u, "m", 1, 1, 0, 0);
    sc_cost_record_usage(&t, &u);
    sc_cost_record_usage(&t, &u);
    SC_ASSERT_EQ(sc_cost_request_count(&t), 2u);
    sc_cost_tracker_deinit(&t);
}

static void test_cost_get_summary_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    sc_cost_summary_t summary;
    sc_cost_get_summary(&t, 0, &summary);
    SC_ASSERT_EQ(summary.request_count, 0u);
    SC_ASSERT_EQ(summary.total_tokens, 0u);
    SC_ASSERT(summary.session_cost_usd == 0.0);
    sc_cost_tracker_deinit(&t);
}

static void test_cost_budget_warning_threshold(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cost_tracker_t t;
    sc_cost_tracker_init(&t, &alloc, "/tmp", true, 20.0, 200.0, 80);
    sc_cost_entry_t u;
    sc_token_usage_init(&u, "m", 10000000, 10000000, 1.0, 2.0);
    sc_cost_record_usage(&t, &u);
    sc_budget_info_t info;
    sc_budget_check_t check = sc_cost_check_budget(&t, 1.0, &info);
    SC_ASSERT(check == SC_BUDGET_WARNING || check == SC_BUDGET_EXCEEDED);
    sc_cost_tracker_deinit(&t);
}

static void test_token_usage_total_tokens(void) {
    sc_cost_entry_t u;
    sc_token_usage_init(&u, "model", 100, 200, 0, 0);
    SC_ASSERT_EQ(u.total_tokens, 300u);
}

static void test_token_usage_cost_zero_prices(void) {
    sc_cost_entry_t u;
    sc_token_usage_init(&u, "model", 1000, 500, 0, 0);
    SC_ASSERT_EQ(u.cost_usd, 0.0);
}

static void test_heartbeat_free_tasks_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_heartbeat_free_tasks(&alloc, NULL, 0);
}

static void test_heartbeat_collect_tasks_missing_file(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, true, 30, "/nonexistent/heartbeat/dir");
    char **tasks = NULL;
    size_t count = 0;
    sc_error_t err = sc_heartbeat_collect_tasks(&e, &alloc, &tasks, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
}

static void test_heartbeat_tick_missing_file(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_heartbeat_engine_t e;
    sc_heartbeat_engine_init(&e, true, 30, "/nonexistent/heartbeat/dir");
    sc_heartbeat_result_t result = {0};
    sc_error_t err = sc_heartbeat_tick(&e, &alloc, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.outcome, SC_HEARTBEAT_SKIPPED_MISSING);
}

void run_infrastructure_tests(void) {
    SC_TEST_SUITE("Infrastructure (version, heartbeat, cost)");
    SC_RUN_TEST(test_version_string);
    SC_RUN_TEST(test_heartbeat_init_clamps_interval);
    SC_RUN_TEST(test_heartbeat_init_preserves_valid_interval);
    SC_RUN_TEST(test_heartbeat_is_content_empty);
    SC_RUN_TEST(test_heartbeat_parse_tasks);
    SC_RUN_TEST(test_heartbeat_parse_tasks_empty);
    SC_RUN_TEST(test_token_usage_init);
    SC_RUN_TEST(test_cost_tracker_init_deinit);
    SC_RUN_TEST(test_cost_tracker_record_and_summary);
    SC_RUN_TEST(test_cost_budget_disabled_allowed);
    SC_RUN_TEST(test_heartbeat_parse_tasks_asterisk_bullet);
    SC_RUN_TEST(test_heartbeat_is_empty_headers_only);
    SC_RUN_TEST(test_heartbeat_is_empty_blank_lines);
    SC_RUN_TEST(test_heartbeat_parse_tasks_skips_empty_bullets);
    SC_RUN_TEST(test_heartbeat_engine_init_clamps_to_five);
    SC_RUN_TEST(test_heartbeat_engine_init_max_interval);
    SC_RUN_TEST(test_heartbeat_engine_disabled);
    SC_RUN_TEST(test_cost_session_total_single);
    SC_RUN_TEST(test_cost_session_tokens_accumulation);
    SC_RUN_TEST(test_cost_request_count);
    SC_RUN_TEST(test_cost_get_summary_empty);
    SC_RUN_TEST(test_cost_budget_warning_threshold);
    SC_RUN_TEST(test_token_usage_total_tokens);
    SC_RUN_TEST(test_token_usage_cost_zero_prices);
    SC_RUN_TEST(test_heartbeat_free_tasks_null_safe);
    SC_RUN_TEST(test_heartbeat_collect_tasks_missing_file);
    SC_RUN_TEST(test_heartbeat_tick_missing_file);
}
