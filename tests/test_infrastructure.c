#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/cost.h"
#include "human/heartbeat.h"
#include "human/version.h"
#include "test_framework.h"
#include <string.h>

static void test_version_string(void) {
    const char *v = hu_version_string();
    HU_ASSERT_NOT_NULL(v);
    HU_ASSERT(strlen(v) > 0);
}

static void test_heartbeat_init_clamps_interval(void) {
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, true, 2, "/tmp");
    HU_ASSERT_EQ(e.interval_minutes, 5u);
}

static void test_heartbeat_init_preserves_valid_interval(void) {
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, true, 30, "/tmp");
    HU_ASSERT_EQ(e.interval_minutes, 30u);
}

static void test_heartbeat_is_content_empty(void) {
    HU_ASSERT_TRUE(hu_heartbeat_is_empty_content(""));
    HU_ASSERT_TRUE(hu_heartbeat_is_empty_content("# HEARTBEAT\n\n# comment"));
    HU_ASSERT_FALSE(hu_heartbeat_is_empty_content("Check status"));
}

static void test_heartbeat_parse_tasks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "# Tasks\n\n- Check email\n- Review calendar\nNot a task\n- Third task";
    char **tasks = NULL;
    size_t count = 0;
    hu_error_t err = hu_heartbeat_parse_tasks(&alloc, content, &tasks, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_STR_EQ(tasks[0], "Check email");
    HU_ASSERT_STR_EQ(tasks[1], "Review calendar");
    HU_ASSERT_STR_EQ(tasks[2], "Third task");
    hu_heartbeat_free_tasks(&alloc, tasks, count);
}

static void test_heartbeat_parse_tasks_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char **tasks = NULL;
    size_t count = 0;
    hu_error_t err = hu_heartbeat_parse_tasks(&alloc, "", &tasks, &count);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(tasks);
}

static void test_token_usage_init(void) {
    hu_cost_entry_t u;
    hu_token_usage_init(&u, "test/model", 1000, 500, 3.0, 15.0);
    HU_ASSERT_EQ(u.input_tokens, 1000ull);
    HU_ASSERT_EQ(u.output_tokens, 500ull);
    HU_ASSERT_EQ(u.total_tokens, 1500ull);
    HU_ASSERT(u.cost_usd > 0.009 && u.cost_usd < 0.012);
}

static void test_cost_tracker_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_error_t err = hu_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    HU_ASSERT_EQ(err, HU_OK);
    hu_cost_tracker_deinit(&t);
}

static void test_cost_tracker_record_and_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_error_t err = hu_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    HU_ASSERT_EQ(err, HU_OK);

    hu_cost_entry_t u;
    hu_token_usage_init(&u, "model", 1000, 500, 1.0, 2.0);
    err = hu_cost_record_usage(&t, &u, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_cost_summary_t summary;
    hu_cost_get_summary(&t, 0, &summary);
    HU_ASSERT_EQ(summary.request_count, 1u);
    HU_ASSERT(summary.session_cost_usd > 0.0);
    HU_ASSERT(summary.total_tokens == 1500);

    hu_cost_tracker_deinit(&t);
}

static void test_cost_budget_disabled_allowed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_error_t err = hu_cost_tracker_init(&t, &alloc, "/tmp", false, 10.0, 100.0, 80);
    HU_ASSERT_EQ(err, HU_OK);
    hu_budget_info_t info;
    hu_budget_check_t check = hu_cost_check_budget(&t, 1000.0, &info);
    HU_ASSERT(check == HU_BUDGET_ALLOWED);
    hu_cost_tracker_deinit(&t);
}

static void test_heartbeat_parse_tasks_asterisk_bullet(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "# Tasks\n\n- Task one\n* Task two\n- Task three";
    char **tasks = NULL;
    size_t count = 0;
    hu_error_t err = hu_heartbeat_parse_tasks(&alloc, content, &tasks, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_STR_EQ(tasks[0], "Task one");
    HU_ASSERT_STR_EQ(tasks[1], "Task three");
    if (tasks)
        hu_heartbeat_free_tasks(&alloc, tasks, count);
}

static void test_heartbeat_is_empty_headers_only(void) {
    HU_ASSERT_TRUE(hu_heartbeat_is_empty_content("# Header\n## Sub\n\n# Another"));
}

static void test_heartbeat_is_empty_blank_lines(void) {
    HU_ASSERT_TRUE(hu_heartbeat_is_empty_content("\n\n\n"));
}

static void test_heartbeat_parse_tasks_skips_empty_bullets(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *content = "- Real task\n- \n- Another real";
    char **tasks = NULL;
    size_t count = 0;
    hu_error_t err = hu_heartbeat_parse_tasks(&alloc, content, &tasks, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);
    if (tasks)
        hu_heartbeat_free_tasks(&alloc, tasks, count);
}

static void test_heartbeat_engine_init_clamps_to_five(void) {
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, true, 0, "/tmp");
    HU_ASSERT_EQ(e.interval_minutes, 5u);
}

static void test_heartbeat_engine_init_max_interval(void) {
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, true, 1440, "/tmp");
    HU_ASSERT_EQ(e.interval_minutes, 1440u);
}

static void test_heartbeat_engine_disabled(void) {
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, false, 30, "/tmp");
    HU_ASSERT_FALSE(e.enabled);
}

static void test_cost_session_total_single(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    hu_cost_entry_t u;
    hu_token_usage_init(&u, "m", 1000, 500, 1.0, 2.0);
    hu_cost_record_usage(&t, &u, 0);
    double total = hu_cost_session_total(&t);
    HU_ASSERT(total > 0.0);
    hu_cost_tracker_deinit(&t);
}

static void test_cost_session_tokens_accumulation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    hu_cost_entry_t u1, u2;
    hu_token_usage_init(&u1, "m", 100, 50, 0, 0);
    hu_token_usage_init(&u2, "m", 200, 100, 0, 0);
    hu_cost_record_usage(&t, &u1, 0);
    hu_cost_record_usage(&t, &u2, 0);
    HU_ASSERT_EQ(hu_cost_session_tokens(&t), 450u);
    hu_cost_tracker_deinit(&t);
}

static void test_cost_request_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    HU_ASSERT_EQ(hu_cost_request_count(&t), 0u);
    hu_cost_entry_t u;
    hu_token_usage_init(&u, "m", 1, 1, 0, 0);
    hu_cost_record_usage(&t, &u, 0);
    hu_cost_record_usage(&t, &u, 0);
    HU_ASSERT_EQ(hu_cost_request_count(&t), 2u);
    hu_cost_tracker_deinit(&t);
}

static void test_cost_get_summary_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_cost_tracker_init(&t, &alloc, "/tmp", true, 10.0, 100.0, 80);
    hu_cost_summary_t summary;
    hu_cost_get_summary(&t, 0, &summary);
    HU_ASSERT_EQ(summary.request_count, 0u);
    HU_ASSERT_EQ(summary.total_tokens, 0u);
    HU_ASSERT(summary.session_cost_usd == 0.0);
    hu_cost_tracker_deinit(&t);
}

static void test_cost_budget_warning_threshold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cost_tracker_t t;
    hu_cost_tracker_init(&t, &alloc, "/tmp", true, 20.0, 200.0, 80);
    hu_cost_entry_t u;
    hu_token_usage_init(&u, "m", 10000000, 10000000, 1.0, 2.0);
    hu_cost_record_usage(&t, &u, 0);
    hu_budget_info_t info;
    hu_budget_check_t check = hu_cost_check_budget(&t, 1.0, &info);
    HU_ASSERT(check == HU_BUDGET_WARNING || check == HU_BUDGET_EXCEEDED);
    hu_cost_tracker_deinit(&t);
}

static void test_token_usage_total_tokens(void) {
    hu_cost_entry_t u;
    hu_token_usage_init(&u, "model", 100, 200, 0, 0);
    HU_ASSERT_EQ(u.total_tokens, 300u);
}

static void test_token_usage_cost_zero_prices(void) {
    hu_cost_entry_t u;
    hu_token_usage_init(&u, "model", 1000, 500, 0, 0);
    HU_ASSERT_EQ(u.cost_usd, 0.0);
}

static void test_heartbeat_free_tasks_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_heartbeat_free_tasks(&alloc, NULL, 0);
}

static void test_heartbeat_collect_tasks_missing_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, true, 30, "/nonexistent/heartbeat/dir");
    char **tasks = NULL;
    size_t count = 0;
    hu_error_t err = hu_heartbeat_collect_tasks(&e, &alloc, &tasks, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);
}

static void test_heartbeat_tick_missing_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_heartbeat_engine_t e;
    hu_heartbeat_engine_init(&e, true, 30, "/nonexistent/heartbeat/dir");
    hu_heartbeat_result_t result = {0};
    hu_error_t err = hu_heartbeat_tick(&e, &alloc, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.outcome, HU_HEARTBEAT_SKIPPED_MISSING);
}

void run_infrastructure_tests(void) {
    HU_TEST_SUITE("Infrastructure (version, heartbeat, cost)");
    HU_RUN_TEST(test_version_string);
    HU_RUN_TEST(test_heartbeat_init_clamps_interval);
    HU_RUN_TEST(test_heartbeat_init_preserves_valid_interval);
    HU_RUN_TEST(test_heartbeat_is_content_empty);
    HU_RUN_TEST(test_heartbeat_parse_tasks);
    HU_RUN_TEST(test_heartbeat_parse_tasks_empty);
    HU_RUN_TEST(test_token_usage_init);
    HU_RUN_TEST(test_cost_tracker_init_deinit);
    HU_RUN_TEST(test_cost_tracker_record_and_summary);
    HU_RUN_TEST(test_cost_budget_disabled_allowed);
    HU_RUN_TEST(test_heartbeat_parse_tasks_asterisk_bullet);
    HU_RUN_TEST(test_heartbeat_is_empty_headers_only);
    HU_RUN_TEST(test_heartbeat_is_empty_blank_lines);
    HU_RUN_TEST(test_heartbeat_parse_tasks_skips_empty_bullets);
    HU_RUN_TEST(test_heartbeat_engine_init_clamps_to_five);
    HU_RUN_TEST(test_heartbeat_engine_init_max_interval);
    HU_RUN_TEST(test_heartbeat_engine_disabled);
    HU_RUN_TEST(test_cost_session_total_single);
    HU_RUN_TEST(test_cost_session_tokens_accumulation);
    HU_RUN_TEST(test_cost_request_count);
    HU_RUN_TEST(test_cost_get_summary_empty);
    HU_RUN_TEST(test_cost_budget_warning_threshold);
    HU_RUN_TEST(test_token_usage_total_tokens);
    HU_RUN_TEST(test_token_usage_cost_zero_prices);
    HU_RUN_TEST(test_heartbeat_free_tasks_null_safe);
    HU_RUN_TEST(test_heartbeat_collect_tasks_missing_file);
    HU_RUN_TEST(test_heartbeat_tick_missing_file);
}
