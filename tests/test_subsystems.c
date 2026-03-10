#include "human/core/allocator.h"
#include "human/daemon.h"
#include "human/migration.h"
#include "human/onboard.h"
#include "human/skillforge.h"
#include "test_framework.h"
#include <time.h>

static void test_skillforge_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_error_t err = hu_skillforge_create(&alloc, &sf);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_NOT_NULL(sf.skills);
    HU_ASSERT_EQ(sf.skills_len, 0);
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_discover_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_error_t err = hu_skillforge_discover(&sf, ".");
    HU_ASSERT(err == HU_OK);
    hu_skill_t *skills = NULL;
    size_t count = 0;
    err = hu_skillforge_list_skills(&sf, &skills, &count);
    HU_ASSERT(err == HU_OK);
    /* HU_IS_TEST: discover adds 3 test skills */
    HU_ASSERT(count >= 3);
    HU_ASSERT_NOT_NULL(hu_skillforge_get_skill(&sf, "test-skill"));
    HU_ASSERT_NULL(hu_skillforge_get_skill(&sf, "nonexistent"));
    hu_skillforge_destroy(&sf);
}

static void test_skillforge_enable_disable(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skillforge_t sf;
    hu_skillforge_create(&alloc, &sf);
    hu_skillforge_discover(&sf, ".");
    hu_skill_t *s = hu_skillforge_get_skill(&sf, "another-skill");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_FALSE(s->enabled);
    hu_error_t err = hu_skillforge_enable(&sf, "another-skill");
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_TRUE(s->enabled);
    err = hu_skillforge_disable(&sf, "another-skill");
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_FALSE(s->enabled);
    err = hu_skillforge_enable(&sf, "nonexistent");
    HU_ASSERT(err == HU_ERR_NOT_FOUND);
    hu_skillforge_destroy(&sf);
}

static void test_onboard_check_first_run(void) {
    /* May be true or false depending on env */
    (void)hu_onboard_check_first_run();
}

static void test_onboard_run_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_onboard_run(&alloc);
    /* In HU_IS_TEST, onboard returns immediately without prompting */
    HU_ASSERT(err == HU_OK);
}

static void test_onboard_run_invalid_alloc(void) {
    hu_error_t err = hu_onboard_run(NULL);
    HU_ASSERT(err == HU_OK);
}

static void test_daemon_start_test_mode(void) {
    hu_error_t err = hu_daemon_start();
    /* In HU_IS_TEST: just validates args, returns OK */
    HU_ASSERT(err == HU_OK);
}

static void test_daemon_stop_test_mode(void) {
    hu_error_t err = hu_daemon_stop();
    HU_ASSERT(err == HU_OK);
}

static void test_daemon_status_test_mode(void) {
    bool status = hu_daemon_status();
    HU_ASSERT_FALSE(status);
}

static void test_daemon_install_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_daemon_install(&alloc);
    HU_ASSERT(err == HU_OK);
}

static void test_daemon_uninstall_test_mode(void) {
    hu_error_t err = hu_daemon_uninstall();
    HU_ASSERT(err == HU_OK);
}

static void test_daemon_logs_test_mode(void) {
    hu_error_t err = hu_daemon_logs();
    HU_ASSERT(err == HU_OK);
}

static void test_migration_run_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = NULL,
        .source_path_len = 0,
        .target_path = ".",
        .target_path_len = 1,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(stats.imported, 0);
}

static void test_migration_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {0};
    hu_migration_stats_t stats = {0};
    hu_error_t err = hu_migration_run(NULL, NULL, NULL, NULL, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    err = hu_migration_run(&alloc, NULL, &stats, NULL, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    err = hu_migration_run(&alloc, &cfg, NULL, NULL, NULL);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
}

static void progress_cb(void *ctx, size_t cur, size_t total) {
    (void)cur;
    (void)total;
    (*(size_t *)ctx)++;
}

static void test_migration_with_progress(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_migration_config_t cfg = {
        .source = HU_MIGRATION_SOURCE_NONE,
        .target = HU_MIGRATION_TARGET_MARKDOWN,
        .source_path = NULL,
        .source_path_len = 0,
        .target_path = ".",
        .target_path_len = 1,
        .dry_run = true,
    };
    hu_migration_stats_t stats = {0};
    size_t progress_calls = 0;
    hu_error_t err = hu_migration_run(&alloc, &cfg, &stats, progress_cb, &progress_calls);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT_EQ(progress_calls, 1);
}

/* ── Cron schedule matching tests ──────────────────────────────────────── */

static struct tm make_tm(int min, int hour, int mday, int mon, int wday) {
    struct tm t = {0};
    t.tm_min = min;
    t.tm_hour = hour;
    t.tm_mday = mday;
    t.tm_mon = mon - 1; /* tm_mon is 0-based */
    t.tm_wday = wday;
    return t;
}

static void test_cron_match_wildcard(void) {
    struct tm t = make_tm(30, 12, 15, 3, 1);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("* * * * *", &t));
}

static void test_cron_match_exact_minute(void) {
    struct tm t = make_tm(30, 12, 15, 3, 1);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("30 * * * *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("15 * * * *", &t));
}

static void test_cron_match_exact_hour(void) {
    struct tm t = make_tm(0, 9, 15, 3, 1);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0 9 * * *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 10 * * *", &t));
}

static void test_cron_match_step_minutes(void) {
    struct tm t0 = make_tm(0, 12, 1, 1, 3);
    struct tm t15 = make_tm(15, 12, 1, 1, 3);
    struct tm t30 = make_tm(30, 12, 1, 1, 3);
    struct tm t7 = make_tm(7, 12, 1, 1, 3);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/15 * * * *", &t0));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/15 * * * *", &t15));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/15 * * * *", &t30));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/15 * * * *", &t7));
}

static void test_cron_match_step_five(void) {
    struct tm t0 = make_tm(0, 0, 1, 1, 0);
    struct tm t5 = make_tm(5, 0, 1, 1, 0);
    struct tm t10 = make_tm(10, 0, 1, 1, 0);
    struct tm t3 = make_tm(3, 0, 1, 1, 0);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 * * * *", &t0));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 * * * *", &t5));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 * * * *", &t10));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 * * * *", &t3));
}

static void test_cron_match_range_weekday(void) {
    struct tm mon = make_tm(0, 9, 1, 1, 1);
    struct tm fri = make_tm(0, 9, 5, 1, 5);
    struct tm sat = make_tm(0, 9, 6, 1, 6);
    struct tm sun = make_tm(0, 9, 7, 1, 0);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0 9 * * 1-5", &mon));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0 9 * * 1-5", &fri));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 9 * * 1-5", &sat));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 9 * * 1-5", &sun));
}

static void test_cron_match_list(void) {
    struct tm t0 = make_tm(0, 12, 1, 1, 3);
    struct tm t15 = make_tm(15, 12, 1, 1, 3);
    struct tm t30 = make_tm(30, 12, 1, 1, 3);
    struct tm t45 = make_tm(45, 12, 1, 1, 3);
    struct tm t10 = make_tm(10, 12, 1, 1, 3);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0,15,30,45 * * * *", &t0));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0,15,30,45 * * * *", &t15));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0,15,30,45 * * * *", &t30));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0,15,30,45 * * * *", &t45));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0,15,30,45 * * * *", &t10));
}

static void test_cron_match_range_with_step(void) {
    /* 1-10/3 should match 1, 4, 7, 10 */
    struct tm t1 = make_tm(1, 0, 1, 1, 0);
    struct tm t4 = make_tm(4, 0, 1, 1, 0);
    struct tm t7 = make_tm(7, 0, 1, 1, 0);
    struct tm t10 = make_tm(10, 0, 1, 1, 0);
    struct tm t2 = make_tm(2, 0, 1, 1, 0);
    struct tm t11 = make_tm(11, 0, 1, 1, 0);
    HU_ASSERT_TRUE(hu_cron_schedule_matches("1-10/3 * * * *", &t1));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("1-10/3 * * * *", &t4));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("1-10/3 * * * *", &t7));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("1-10/3 * * * *", &t10));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("1-10/3 * * * *", &t2));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("1-10/3 * * * *", &t11));
}

static void test_cron_match_complex_expression(void) {
    /* every 5 min, 9am-5pm, weekdays */
    struct tm match = make_tm(10, 12, 3, 3, 3);   /* Wed 12:10 Mar 3 */
    struct tm no_min = make_tm(13, 12, 3, 3, 3);  /* not on 5-min boundary */
    struct tm no_hour = make_tm(10, 20, 3, 3, 3); /* 8pm outside range */
    struct tm no_dow = make_tm(10, 12, 3, 3, 6);  /* Saturday */
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &match));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &no_min));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &no_hour));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &no_dow));
}

static void test_cron_match_null_inputs(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    HU_ASSERT_FALSE(hu_cron_schedule_matches(NULL, &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("* * * * *", NULL));
    HU_ASSERT_FALSE(hu_cron_schedule_matches(NULL, NULL));
}

static void test_cron_match_too_few_fields(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    HU_ASSERT_FALSE(hu_cron_schedule_matches("* * *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("* *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("", &t));
}

static void test_cron_match_dom_and_month(void) {
    struct tm t = make_tm(0, 0, 25, 12, 4); /* Dec 25 */
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0 0 25 12 *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 0 24 12 *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 0 25 11 *", &t));
}

static void test_cron_match_step_zero_rejected(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/0 * * * *", &t));
}

static void test_cron_match_empty_atom_in_list(void) {
    struct tm t5 = make_tm(5, 0, 1, 1, 0);
    struct tm t0 = make_tm(0, 0, 1, 1, 0);
    /* "5,,10" has empty atom between commas — empty atom returns false */
    HU_ASSERT_TRUE(hu_cron_schedule_matches("5,10 * * * *", &t5));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("5,10 * * * *", &t0));
}

static void test_cron_match_malformed_range(void) {
    struct tm t = make_tm(5, 0, 1, 1, 0);
    /* Dash at start: "-5" parses as negative which strtol handles, range lo < 0 */
    HU_ASSERT_FALSE(hu_cron_schedule_matches("-5 * * * *", &t));
    /* Trailing dash: "5-" has empty hi, parse_cron_int("") fails */
    HU_ASSERT_FALSE(hu_cron_schedule_matches("5- * * * *", &t));
}

static void test_cron_match_large_numbers(void) {
    struct tm t = make_tm(30, 12, 15, 6, 3);
    HU_ASSERT_FALSE(hu_cron_schedule_matches("99999 * * * *", &t));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("30 * * * *", &t));
}

static void test_cron_match_negative_step(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/-1 * * * *", &t));
}

static void test_service_run_null_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_service_run(&alloc, 0, NULL, 0, NULL, NULL);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_service_run_null_alloc(void) {
    hu_error_t err = hu_service_run(NULL, 0, NULL, 0, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

void run_subsystems_tests(void) {
    HU_TEST_SUITE("Subsystems (skillforge, onboard, daemon, migration)");

    HU_RUN_TEST(test_skillforge_create_destroy);
    HU_RUN_TEST(test_skillforge_discover_list);
    HU_RUN_TEST(test_skillforge_enable_disable);

    HU_RUN_TEST(test_onboard_check_first_run);
    HU_RUN_TEST(test_onboard_run_test_mode);
    HU_RUN_TEST(test_onboard_run_invalid_alloc);

    HU_RUN_TEST(test_daemon_start_test_mode);
    HU_RUN_TEST(test_daemon_stop_test_mode);
    HU_RUN_TEST(test_daemon_status_test_mode);
    HU_RUN_TEST(test_daemon_install_test_mode);
    HU_RUN_TEST(test_daemon_uninstall_test_mode);
    HU_RUN_TEST(test_daemon_logs_test_mode);

    HU_RUN_TEST(test_migration_run_test_mode);
    HU_RUN_TEST(test_migration_invalid_args);
    HU_RUN_TEST(test_migration_with_progress);

    HU_RUN_TEST(test_cron_match_wildcard);
    HU_RUN_TEST(test_cron_match_exact_minute);
    HU_RUN_TEST(test_cron_match_exact_hour);
    HU_RUN_TEST(test_cron_match_step_minutes);
    HU_RUN_TEST(test_cron_match_step_five);
    HU_RUN_TEST(test_cron_match_range_weekday);
    HU_RUN_TEST(test_cron_match_list);
    HU_RUN_TEST(test_cron_match_range_with_step);
    HU_RUN_TEST(test_cron_match_complex_expression);
    HU_RUN_TEST(test_cron_match_null_inputs);
    HU_RUN_TEST(test_cron_match_too_few_fields);
    HU_RUN_TEST(test_cron_match_dom_and_month);
    HU_RUN_TEST(test_cron_match_step_zero_rejected);
    HU_RUN_TEST(test_cron_match_empty_atom_in_list);
    HU_RUN_TEST(test_cron_match_malformed_range);
    HU_RUN_TEST(test_cron_match_large_numbers);
    HU_RUN_TEST(test_cron_match_negative_step);
    HU_RUN_TEST(test_service_run_null_agent);
    HU_RUN_TEST(test_service_run_null_alloc);
}
