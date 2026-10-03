#include "human/agent.h"
#include "human/channel.h"
#include "human/cron.h"
#include "human/daemon.h"
#include "human/daemon_cron.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#endif
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── hu_cron_atom_matches ────────────────────────────────────────────── */

static void test_atom_matches_exact_value(void) {
    HU_ASSERT_TRUE(hu_cron_atom_matches("5", 1, 5));
    HU_ASSERT_FALSE(hu_cron_atom_matches("5", 1, 6));
}

static void test_atom_matches_star(void) {
    HU_ASSERT_TRUE(hu_cron_atom_matches("*", 1, 0));
    HU_ASSERT_TRUE(hu_cron_atom_matches("*", 1, 59));
}

static void test_atom_matches_step(void) {
    HU_ASSERT_TRUE(hu_cron_atom_matches("*/5", 3, 0));
    HU_ASSERT_TRUE(hu_cron_atom_matches("*/5", 3, 10));
    HU_ASSERT_FALSE(hu_cron_atom_matches("*/5", 3, 3));
}

static void test_atom_matches_range(void) {
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-5", 3, 1));
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-5", 3, 3));
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-5", 3, 5));
    HU_ASSERT_FALSE(hu_cron_atom_matches("1-5", 3, 0));
    HU_ASSERT_FALSE(hu_cron_atom_matches("1-5", 3, 6));
}

static void test_atom_matches_range_step(void) {
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-10/3", 6, 1));
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-10/3", 6, 4));
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-10/3", 6, 7));
    HU_ASSERT_TRUE(hu_cron_atom_matches("1-10/3", 6, 10));
    HU_ASSERT_FALSE(hu_cron_atom_matches("1-10/3", 6, 2));
}

static void test_atom_matches_zero_len(void) {
    HU_ASSERT_FALSE(hu_cron_atom_matches("5", 0, 5));
}

static void test_atom_matches_invalid_step(void) {
    HU_ASSERT_FALSE(hu_cron_atom_matches("*/0", 3, 0));
    HU_ASSERT_FALSE(hu_cron_atom_matches("*/-1", 4, 0));
}

static void test_atom_matches_overflow_len(void) {
    /* Atom longer than 32 chars should fail */
    char big[40];
    memset(big, '1', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    HU_ASSERT_FALSE(hu_cron_atom_matches(big, sizeof(big) - 1, 1));
}

/* ── hu_cron_field_matches ───────────────────────────────────────────── */

static void test_field_matches_star(void) {
    HU_ASSERT_TRUE(hu_cron_field_matches("*", 42));
}

static void test_field_matches_exact(void) {
    HU_ASSERT_TRUE(hu_cron_field_matches("30", 30));
    HU_ASSERT_FALSE(hu_cron_field_matches("30", 15));
}

static void test_field_matches_comma_list(void) {
    HU_ASSERT_TRUE(hu_cron_field_matches("0,15,30,45", 15));
    HU_ASSERT_TRUE(hu_cron_field_matches("0,15,30,45", 45));
    HU_ASSERT_FALSE(hu_cron_field_matches("0,15,30,45", 10));
}

static void test_field_matches_null(void) {
    HU_ASSERT_FALSE(hu_cron_field_matches(NULL, 5));
}

/* ── hu_cron_schedule_matches ────────────────────────────────────────── */

static void test_schedule_matches_all_star(void) {
    struct tm t = {.tm_min = 30, .tm_hour = 14, .tm_mday = 15, .tm_mon = 5, .tm_wday = 3};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("* * * * *", &t));
}

static void test_schedule_matches_exact_time(void) {
    struct tm t = {.tm_min = 30, .tm_hour = 14, .tm_mday = 15, .tm_mon = 5, .tm_wday = 3};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("30 14 15 6 3", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 9 * * *", &t));
}

static void test_schedule_matches_step_minutes(void) {
    struct tm t0 = {.tm_min = 0, .tm_hour = 10, .tm_mday = 1, .tm_mon = 0, .tm_wday = 1};
    struct tm t5 = {.tm_min = 5, .tm_hour = 10, .tm_mday = 1, .tm_mon = 0, .tm_wday = 1};
    struct tm t3 = {.tm_min = 3, .tm_hour = 10, .tm_mday = 1, .tm_mon = 0, .tm_wday = 1};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 * * * *", &t0));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 * * * *", &t5));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 * * * *", &t3));
}

static void test_schedule_matches_weekday_range(void) {
    struct tm mon = {.tm_min = 0, .tm_hour = 9, .tm_mday = 1, .tm_mon = 0, .tm_wday = 1};
    struct tm sat = {.tm_min = 0, .tm_hour = 9, .tm_mday = 1, .tm_mon = 0, .tm_wday = 6};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0 9 * * 1-5", &mon));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 9 * * 1-5", &sat));
}

static void test_schedule_matches_null_input(void) {
    struct tm t = {0};
    HU_ASSERT_FALSE(hu_cron_schedule_matches(NULL, &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("* * * * *", NULL));
    HU_ASSERT_FALSE(hu_cron_schedule_matches(NULL, NULL));
}

static void test_schedule_matches_too_few_fields(void) {
    struct tm t = {0};
    HU_ASSERT_FALSE(hu_cron_schedule_matches("* * *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("", &t));
}

static void test_schedule_matches_specific_date(void) {
    struct tm t = {.tm_min = 0, .tm_hour = 0, .tm_mday = 25, .tm_mon = 11, .tm_wday = 3};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("0 0 25 12 *", &t));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("0 0 24 12 *", &t));
}

static void test_schedule_matches_comma_minutes(void) {
    struct tm t5 = {.tm_min = 5, .tm_hour = 12, .tm_mday = 1, .tm_mon = 0, .tm_wday = 0};
    struct tm t10 = {.tm_min = 10, .tm_hour = 12, .tm_mday = 1, .tm_mon = 0, .tm_wday = 0};
    struct tm t7 = {.tm_min = 7, .tm_hour = 12, .tm_mday = 1, .tm_mon = 0, .tm_wday = 0};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("5,10 * * * *", &t5));
    HU_ASSERT_TRUE(hu_cron_schedule_matches("5,10 * * * *", &t10));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("5,10 * * * *", &t7));
}

static void test_schedule_matches_range_step_combined(void) {
    /* Business hours: every 5 min, 9-17, Mon-Fri */
    struct tm match = {.tm_min = 10, .tm_hour = 12, .tm_mday = 1, .tm_mon = 0, .tm_wday = 3};
    struct tm no_min = {.tm_min = 3, .tm_hour = 12, .tm_mday = 1, .tm_mon = 0, .tm_wday = 3};
    struct tm no_dow = {.tm_min = 10, .tm_hour = 12, .tm_mday = 1, .tm_mon = 0, .tm_wday = 6};
    HU_ASSERT_TRUE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &match));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &no_min));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("*/5 9-17 * * 1-5", &no_dow));
}

/* ── hu_daemon_cron_tick ─────────────────────────────────────────────── */

static void test_cron_tick_null_alloc(void) {
    /* Should not crash with NULL allocator */
    hu_daemon_cron_tick(NULL);
}

static void test_cron_tick_runs_without_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* In HU_IS_TEST mode this is a no-op since run_cron_tick skips execution */
    hu_daemon_cron_tick(&alloc);
}

/* ── proactive check-ins ──────────────────────────────────────────────────────
 * 2026-10-02: the persona stores a contact's proactive channel with the handle
 * already in it ("imessage:<handle>"); registration appended contact_id again, so
 * every daily check-in targeted "imessage:<handle>:<handle>" and never sent. */

static void checkin_target_uses_channel_that_already_names_the_handle(void) {
    char buf[64];
    int n = hu_proactive_checkin_target(buf, sizeof(buf), "imessage:+15550001111", "+15550001111");
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, "imessage:+15550001111");
}

static void checkin_target_appends_contact_to_a_bare_channel(void) {
    char buf[64];
    HU_ASSERT_TRUE(hu_proactive_checkin_target(buf, sizeof(buf), "imessage", "+15550001111") > 0);
    HU_ASSERT_STR_EQ(buf, "imessage:+15550001111");
}

static void checkin_target_rejects_bad_input_and_truncation(void) {
    char buf[8];
    HU_ASSERT_EQ(hu_proactive_checkin_target(buf, sizeof(buf), NULL, "x"), -1);
    HU_ASSERT_EQ(buf[0], '\0');
    HU_ASSERT_EQ(hu_proactive_checkin_target(buf, sizeof(buf), "imessage", NULL), -1);
    HU_ASSERT_EQ(hu_proactive_checkin_target(buf, sizeof(buf), "imessage", "+15550001111"), -1);
    HU_ASSERT_EQ(buf[0], '\0');
    HU_ASSERT_EQ(hu_proactive_checkin_target(NULL, 8, "imessage", "x"), -1);
}

static void checkin_job_name_predicate(void) {
    HU_ASSERT_TRUE(hu_cron_job_is_proactive_checkin("proactive:Mom"));
    HU_ASSERT_FALSE(hu_cron_job_is_proactive_checkin("research-agent"));
    HU_ASSERT_FALSE(hu_cron_job_is_proactive_checkin("proactiveX"));
    HU_ASSERT_FALSE(hu_cron_job_is_proactive_checkin(NULL));
}

static void checkin_mode_reads_env_default_off(void) {
    const char *prev = getenv("HU_PROACTIVE_CHECKINS");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HU_PROACTIVE_CHECKINS");
    HU_ASSERT_EQ((int)hu_proactive_checkin_mode(), (int)HU_GATE_OFF);
    setenv("HU_PROACTIVE_CHECKINS", "shadow", 1);
    HU_ASSERT_EQ((int)hu_proactive_checkin_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_PROACTIVE_CHECKINS", "live", 1);
    HU_ASSERT_EQ((int)hu_proactive_checkin_mode(), (int)HU_GATE_LIVE);
    if (saved) {
        setenv("HU_PROACTIVE_CHECKINS", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_PROACTIVE_CHECKINS");
    }
}

/* The runner end to end: an every-minute check-in job and a mock iMessage channel
 * that records sends. Under HU_IS_TEST the agent turn returns canned text. */
typedef struct cron_mock_channel {
    size_t sends;
    char last_target[64];
} cron_mock_channel_t;

static hu_error_t cron_mock_send(void *ctx, const char *target, size_t target_len,
                                 const char *message, size_t message_len, const char *const *media,
                                 size_t media_count) {
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    cron_mock_channel_t *m = (cron_mock_channel_t *)ctx;
    m->sends++;
    size_t n = target_len < sizeof(m->last_target) - 1 ? target_len : sizeof(m->last_target) - 1;
    memcpy(m->last_target, target, n);
    m->last_target[n] = '\0';
    return HU_OK;
}

static const char *cron_mock_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static const hu_channel_vtable_t cron_mock_vtable = {.send = cron_mock_send,
                                                     .name = cron_mock_name};

static size_t run_checkin_under(const char *mode, char *target_out, size_t target_cap) {
    const char *prev = getenv("HU_PROACTIVE_CHECKINS");
    char *saved = prev ? strdup(prev) : NULL;
    if (mode)
        setenv("HU_PROACTIVE_CHECKINS", mode, 1);
    else
        unsetenv("HU_PROACTIVE_CHECKINS");

    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 8, true);
    HU_ASSERT_NOT_NULL(sched);
    char target[64];
    HU_ASSERT_TRUE(hu_proactive_checkin_target(target, sizeof(target), "imessage:+15550001111",
                                               "+15550001111") > 0);
    uint64_t id = 0;
    HU_ASSERT_EQ(hu_cron_add_agent_job(sched, &alloc, "* * * * *", "check in", target,
                                       "proactive:Test", &id),
                 HU_OK);

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.scheduler = sched;
#ifdef HU_ENABLE_SQLITE
    /* Contact-targeted cron sends also pass the unprompted gate stack (#597),
     * which fails closed without a ledger: give it one. */
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    agent.memory = &mem;
#endif
    cron_mock_channel_t mock;
    memset(&mock, 0, sizeof(mock));
    hu_channel_t ch = {.ctx = &mock, .vtable = &cron_mock_vtable};
    hu_service_channel_t svc;
    memset(&svc, 0, sizeof(svc));
    svc.channel = &ch;

    /* Local noon: quiet hours must not decide this test by wall-clock time. */
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    HU_ASSERT_EQ(hu_service_run_agent_cron_at(&alloc, &agent, &svc, 1, mktime(&tm)), HU_OK);
    if (target_out && target_cap)
        snprintf(target_out, target_cap, "%s", mock.last_target);
    hu_cron_destroy(sched, &alloc);
#ifdef HU_ENABLE_SQLITE
    mem.vtable->deinit(mem.ctx);
#endif

    if (saved) {
        setenv("HU_PROACTIVE_CHECKINS", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_PROACTIVE_CHECKINS");
    }
    return mock.sends;
}

static void checkin_runner_off_sends_nothing(void) {
    HU_ASSERT_EQ(run_checkin_under(NULL, NULL, 0), 0u);
    HU_ASSERT_EQ(run_checkin_under("off", NULL, 0), 0u);
}

static void checkin_runner_shadow_writes_but_sends_nothing(void) {
    HU_ASSERT_EQ(run_checkin_under("shadow", NULL, 0), 0u);
}

static void checkin_runner_live_sends_to_the_single_handle(void) {
    char target[64] = {0};
#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_EQ(run_checkin_under("live", target, sizeof(target)), 1u);
    HU_ASSERT_STR_EQ(target, "+15550001111");
#else
    /* Without SQLite the unprompted stack has no ledger and fails closed. */
    HU_ASSERT_EQ(run_checkin_under("live", target, sizeof(target)), 0u);
#endif
}

void run_daemon_cron_tests(void) {
    HU_TEST_SUITE("daemon_cron");

    /* atom matching */
    HU_RUN_TEST(test_atom_matches_exact_value);
    HU_RUN_TEST(test_atom_matches_star);
    HU_RUN_TEST(test_atom_matches_step);
    HU_RUN_TEST(test_atom_matches_range);
    HU_RUN_TEST(test_atom_matches_range_step);
    HU_RUN_TEST(test_atom_matches_zero_len);
    HU_RUN_TEST(test_atom_matches_invalid_step);
    HU_RUN_TEST(test_atom_matches_overflow_len);

    /* field matching */
    HU_RUN_TEST(test_field_matches_star);
    HU_RUN_TEST(test_field_matches_exact);
    HU_RUN_TEST(test_field_matches_comma_list);
    HU_RUN_TEST(test_field_matches_null);

    /* schedule matching */
    HU_RUN_TEST(test_schedule_matches_all_star);
    HU_RUN_TEST(test_schedule_matches_exact_time);
    HU_RUN_TEST(test_schedule_matches_step_minutes);
    HU_RUN_TEST(test_schedule_matches_weekday_range);
    HU_RUN_TEST(test_schedule_matches_null_input);
    HU_RUN_TEST(test_schedule_matches_too_few_fields);
    HU_RUN_TEST(test_schedule_matches_specific_date);
    HU_RUN_TEST(test_schedule_matches_comma_minutes);
    HU_RUN_TEST(test_schedule_matches_range_step_combined);

    /* cron tick */
    HU_RUN_TEST(test_cron_tick_null_alloc);
    HU_RUN_TEST(test_cron_tick_runs_without_crash);

    /* proactive check-ins */
    HU_RUN_TEST(checkin_target_uses_channel_that_already_names_the_handle);
    HU_RUN_TEST(checkin_target_appends_contact_to_a_bare_channel);
    HU_RUN_TEST(checkin_target_rejects_bad_input_and_truncation);
    HU_RUN_TEST(checkin_job_name_predicate);
    HU_RUN_TEST(checkin_mode_reads_env_default_off);
    HU_RUN_TEST(checkin_runner_off_sends_nothing);
    HU_RUN_TEST(checkin_runner_shadow_writes_but_sends_nothing);
    HU_RUN_TEST(checkin_runner_live_sends_to_the_single_handle);
}
