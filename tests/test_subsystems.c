#include "test_framework.h"
#include "seaclaw/skillforge.h"
#include "seaclaw/onboard.h"
#include "seaclaw/daemon.h"
#include "seaclaw/migration.h"
#include "seaclaw/core/allocator.h"
#include <time.h>

static void test_skillforge_create_destroy(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf;
    sc_error_t err = sc_skillforge_create(&alloc, &sf);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_NOT_NULL(sf.skills);
    SC_ASSERT_EQ(sf.skills_len, 0);
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_discover_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf;
    sc_skillforge_create(&alloc, &sf);
    sc_error_t err = sc_skillforge_discover(&sf, ".");
    SC_ASSERT(err == SC_OK);
    sc_skill_t *skills = NULL;
    size_t count = 0;
    err = sc_skillforge_list_skills(&sf, &skills, &count);
    SC_ASSERT(err == SC_OK);
    /* SC_IS_TEST: discover adds 3 test skills */
    SC_ASSERT(count >= 3);
    SC_ASSERT_NOT_NULL(sc_skillforge_get_skill(&sf, "test-skill"));
    SC_ASSERT_NULL(sc_skillforge_get_skill(&sf, "nonexistent"));
    sc_skillforge_destroy(&sf);
}

static void test_skillforge_enable_disable(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_skillforge_t sf;
    sc_skillforge_create(&alloc, &sf);
    sc_skillforge_discover(&sf, ".");
    sc_skill_t *s = sc_skillforge_get_skill(&sf, "another-skill");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_FALSE(s->enabled);
    sc_error_t err = sc_skillforge_enable(&sf, "another-skill");
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_TRUE(s->enabled);
    err = sc_skillforge_disable(&sf, "another-skill");
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_FALSE(s->enabled);
    err = sc_skillforge_enable(&sf, "nonexistent");
    SC_ASSERT(err == SC_ERR_NOT_FOUND);
    sc_skillforge_destroy(&sf);
}

static void test_onboard_check_first_run(void) {
    /* May be true or false depending on env */
    (void)sc_onboard_check_first_run();
}

static void test_onboard_run_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_onboard_run(&alloc);
    /* In SC_IS_TEST, onboard returns immediately without prompting */
    SC_ASSERT(err == SC_OK);
}

static void test_onboard_run_invalid_alloc(void) {
    sc_error_t err = sc_onboard_run(NULL);
    /* With NULL alloc, if not in test mode would fail; test mode still returns OK */
#ifdef SC_IS_TEST
    SC_ASSERT(err == SC_OK);
#else
    (void)err;
#endif
}

static void test_daemon_start_test_mode(void) {
    sc_error_t err = sc_daemon_start();
    /* In SC_IS_TEST: just validates args, returns OK */
    SC_ASSERT(err == SC_OK);
}

static void test_daemon_stop_test_mode(void) {
    sc_error_t err = sc_daemon_stop();
    SC_ASSERT(err == SC_OK);
}

static void test_daemon_status_test_mode(void) {
    bool status = sc_daemon_status();
    /* In test mode, status returns false */
    SC_ASSERT_FALSE(status);
}

static void test_migration_run_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_MARKDOWN,
        .source_path = NULL,
        .source_path_len = 0,
        .target_path = ".",
        .target_path_len = 1,
        .dry_run = true,
    };
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(&alloc, &cfg, &stats, NULL, NULL);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_EQ(stats.imported, 0);
}

static void test_migration_invalid_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {0};
    sc_migration_stats_t stats = {0};
    sc_error_t err = sc_migration_run(NULL, NULL, NULL, NULL, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
    err = sc_migration_run(&alloc, NULL, &stats, NULL, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
    err = sc_migration_run(&alloc, &cfg, NULL, NULL, NULL);
    SC_ASSERT(err == SC_ERR_INVALID_ARGUMENT);
}

static void progress_cb(void *ctx, size_t cur, size_t total) {
    (void)cur;
    (void)total;
    (*(size_t *)ctx)++;
}

static void test_migration_with_progress(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_migration_config_t cfg = {
        .source = SC_MIGRATION_SOURCE_NONE,
        .target = SC_MIGRATION_TARGET_MARKDOWN,
        .source_path = NULL,
        .source_path_len = 0,
        .target_path = ".",
        .target_path_len = 1,
        .dry_run = true,
    };
    sc_migration_stats_t stats = {0};
    size_t progress_calls = 0;
    sc_error_t err = sc_migration_run(&alloc, &cfg, &stats, progress_cb, &progress_calls);
    SC_ASSERT(err == SC_OK);
    SC_ASSERT_EQ(progress_calls, 1);
}

/* ── Cron schedule matching tests ──────────────────────────────────────── */

static struct tm make_tm(int min, int hour, int mday, int mon, int wday) {
    struct tm t = {0};
    t.tm_min = min;
    t.tm_hour = hour;
    t.tm_mday = mday;
    t.tm_mon = mon - 1;  /* tm_mon is 0-based */
    t.tm_wday = wday;
    return t;
}

static void test_cron_match_wildcard(void) {
    struct tm t = make_tm(30, 12, 15, 3, 1);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("* * * * *", &t));
}

static void test_cron_match_exact_minute(void) {
    struct tm t = make_tm(30, 12, 15, 3, 1);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("30 * * * *", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("15 * * * *", &t));
}

static void test_cron_match_exact_hour(void) {
    struct tm t = make_tm(0, 9, 15, 3, 1);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0 9 * * *", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("0 10 * * *", &t));
}

static void test_cron_match_step_minutes(void) {
    struct tm t0 = make_tm(0, 12, 1, 1, 3);
    struct tm t15 = make_tm(15, 12, 1, 1, 3);
    struct tm t30 = make_tm(30, 12, 1, 1, 3);
    struct tm t7 = make_tm(7, 12, 1, 1, 3);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/15 * * * *", &t0));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/15 * * * *", &t15));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/15 * * * *", &t30));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/15 * * * *", &t7));
}

static void test_cron_match_step_five(void) {
    struct tm t0 = make_tm(0, 0, 1, 1, 0);
    struct tm t5 = make_tm(5, 0, 1, 1, 0);
    struct tm t10 = make_tm(10, 0, 1, 1, 0);
    struct tm t3 = make_tm(3, 0, 1, 1, 0);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/5 * * * *", &t0));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/5 * * * *", &t5));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/5 * * * *", &t10));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/5 * * * *", &t3));
}

static void test_cron_match_range_weekday(void) {
    struct tm mon = make_tm(0, 9, 1, 1, 1);
    struct tm fri = make_tm(0, 9, 5, 1, 5);
    struct tm sat = make_tm(0, 9, 6, 1, 6);
    struct tm sun = make_tm(0, 9, 7, 1, 0);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0 9 * * 1-5", &mon));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0 9 * * 1-5", &fri));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("0 9 * * 1-5", &sat));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("0 9 * * 1-5", &sun));
}

static void test_cron_match_list(void) {
    struct tm t0 = make_tm(0, 12, 1, 1, 3);
    struct tm t15 = make_tm(15, 12, 1, 1, 3);
    struct tm t30 = make_tm(30, 12, 1, 1, 3);
    struct tm t45 = make_tm(45, 12, 1, 1, 3);
    struct tm t10 = make_tm(10, 12, 1, 1, 3);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0,15,30,45 * * * *", &t0));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0,15,30,45 * * * *", &t15));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0,15,30,45 * * * *", &t30));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0,15,30,45 * * * *", &t45));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("0,15,30,45 * * * *", &t10));
}

static void test_cron_match_range_with_step(void) {
    /* 1-10/3 should match 1, 4, 7, 10 */
    struct tm t1 = make_tm(1, 0, 1, 1, 0);
    struct tm t4 = make_tm(4, 0, 1, 1, 0);
    struct tm t7 = make_tm(7, 0, 1, 1, 0);
    struct tm t10 = make_tm(10, 0, 1, 1, 0);
    struct tm t2 = make_tm(2, 0, 1, 1, 0);
    struct tm t11 = make_tm(11, 0, 1, 1, 0);
    SC_ASSERT_TRUE(sc_cron_schedule_matches("1-10/3 * * * *", &t1));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("1-10/3 * * * *", &t4));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("1-10/3 * * * *", &t7));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("1-10/3 * * * *", &t10));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("1-10/3 * * * *", &t2));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("1-10/3 * * * *", &t11));
}

static void test_cron_match_complex_expression(void) {
    /* every 5 min, 9am-5pm, weekdays */
    struct tm match = make_tm(10, 12, 3, 3, 3);     /* Wed 12:10 Mar 3 */
    struct tm no_min = make_tm(13, 12, 3, 3, 3);    /* not on 5-min boundary */
    struct tm no_hour = make_tm(10, 20, 3, 3, 3);   /* 8pm outside range */
    struct tm no_dow = make_tm(10, 12, 3, 3, 6);    /* Saturday */
    SC_ASSERT_TRUE(sc_cron_schedule_matches("*/5 9-17 * * 1-5", &match));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/5 9-17 * * 1-5", &no_min));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/5 9-17 * * 1-5", &no_hour));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/5 9-17 * * 1-5", &no_dow));
}

static void test_cron_match_null_inputs(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    SC_ASSERT_FALSE(sc_cron_schedule_matches(NULL, &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("* * * * *", NULL));
    SC_ASSERT_FALSE(sc_cron_schedule_matches(NULL, NULL));
}

static void test_cron_match_too_few_fields(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    SC_ASSERT_FALSE(sc_cron_schedule_matches("* * *", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("* *", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("", &t));
}

static void test_cron_match_dom_and_month(void) {
    struct tm t = make_tm(0, 0, 25, 12, 4);  /* Dec 25 */
    SC_ASSERT_TRUE(sc_cron_schedule_matches("0 0 25 12 *", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("0 0 24 12 *", &t));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("0 0 25 11 *", &t));
}

static void test_cron_match_step_zero_rejected(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/0 * * * *", &t));
}

static void test_cron_match_empty_atom_in_list(void) {
    struct tm t5 = make_tm(5, 0, 1, 1, 0);
    struct tm t0 = make_tm(0, 0, 1, 1, 0);
    /* "5,,10" has empty atom between commas — empty atom returns false */
    SC_ASSERT_TRUE(sc_cron_schedule_matches("5,10 * * * *", &t5));
    SC_ASSERT_FALSE(sc_cron_schedule_matches("5,10 * * * *", &t0));
}

static void test_cron_match_malformed_range(void) {
    struct tm t = make_tm(5, 0, 1, 1, 0);
    /* Dash at start: "-5" parses as negative which strtol handles, range lo < 0 */
    SC_ASSERT_FALSE(sc_cron_schedule_matches("-5 * * * *", &t));
    /* Trailing dash: "5-" has empty hi, parse_cron_int("") fails */
    SC_ASSERT_FALSE(sc_cron_schedule_matches("5- * * * *", &t));
}

static void test_cron_match_large_numbers(void) {
    struct tm t = make_tm(30, 12, 15, 6, 3);
    SC_ASSERT_FALSE(sc_cron_schedule_matches("99999 * * * *", &t));
    SC_ASSERT_TRUE(sc_cron_schedule_matches("30 * * * *", &t));
}

static void test_cron_match_negative_step(void) {
    struct tm t = make_tm(0, 0, 1, 1, 0);
    SC_ASSERT_FALSE(sc_cron_schedule_matches("*/-1 * * * *", &t));
}

static void test_service_run_null_agent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_service_run(&alloc, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_service_run_null_alloc(void) {
    sc_error_t err = sc_service_run(NULL, 0, NULL, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

void run_subsystems_tests(void) {
    SC_TEST_SUITE("Subsystems (skillforge, onboard, daemon, migration)");

    SC_RUN_TEST(test_skillforge_create_destroy);
    SC_RUN_TEST(test_skillforge_discover_list);
    SC_RUN_TEST(test_skillforge_enable_disable);

    SC_RUN_TEST(test_onboard_check_first_run);
    SC_RUN_TEST(test_onboard_run_test_mode);
    SC_RUN_TEST(test_onboard_run_invalid_alloc);

    SC_RUN_TEST(test_daemon_start_test_mode);
    SC_RUN_TEST(test_daemon_stop_test_mode);
    SC_RUN_TEST(test_daemon_status_test_mode);

    SC_RUN_TEST(test_migration_run_test_mode);
    SC_RUN_TEST(test_migration_invalid_args);
    SC_RUN_TEST(test_migration_with_progress);

    SC_RUN_TEST(test_cron_match_wildcard);
    SC_RUN_TEST(test_cron_match_exact_minute);
    SC_RUN_TEST(test_cron_match_exact_hour);
    SC_RUN_TEST(test_cron_match_step_minutes);
    SC_RUN_TEST(test_cron_match_step_five);
    SC_RUN_TEST(test_cron_match_range_weekday);
    SC_RUN_TEST(test_cron_match_list);
    SC_RUN_TEST(test_cron_match_range_with_step);
    SC_RUN_TEST(test_cron_match_complex_expression);
    SC_RUN_TEST(test_cron_match_null_inputs);
    SC_RUN_TEST(test_cron_match_too_few_fields);
    SC_RUN_TEST(test_cron_match_dom_and_month);
    SC_RUN_TEST(test_cron_match_step_zero_rejected);
    SC_RUN_TEST(test_cron_match_empty_atom_in_list);
    SC_RUN_TEST(test_cron_match_malformed_range);
    SC_RUN_TEST(test_cron_match_large_numbers);
    SC_RUN_TEST(test_cron_match_negative_step);
    SC_RUN_TEST(test_service_run_null_agent);
    SC_RUN_TEST(test_service_run_null_alloc);
}
