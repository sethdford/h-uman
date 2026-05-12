#include "test_framework.h"

#include "human/channel_monitor.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

/* ── mock channel ── */

static bool s_mock_healthy = true;
static int s_mock_start_calls = 0;
static int s_mock_stop_calls = 0;

static hu_error_t mock_start(void *ctx) {
    (void)ctx;
    s_mock_start_calls++;
    return HU_OK;
}
static void mock_stop(void *ctx) {
    (void)ctx;
    s_mock_stop_calls++;
}
static hu_error_t mock_send(void *ctx, const char *t, size_t tl, const char *m, size_t ml,
                            const char *const *media, size_t mc) {
    (void)ctx;
    (void)t;
    (void)tl;
    (void)m;
    (void)ml;
    (void)media;
    (void)mc;
    return HU_OK;
}
static const char *mock_name(void *ctx) {
    (void)ctx;
    return "mock-channel";
}
static bool mock_health(void *ctx) {
    (void)ctx;
    return s_mock_healthy;
}

static hu_channel_vtable_t s_mock_vtable = {
    .start = mock_start,
    .stop = mock_stop,
    .send = mock_send,
    .name = mock_name,
    .health_check = mock_health,
};

static hu_channel_t s_mock_channel = {
    .ctx = NULL,
    .vtable = &s_mock_vtable,
};

/* ── allocator ── */

static void *test_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void test_free(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}
static hu_allocator_t s_alloc = {.alloc = test_alloc, .free = test_free};

/* ── tests ── */

static void test_create_destroy(void) {
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    HU_ASSERT_EQ(hu_channel_monitor_create(&s_alloc, &cfg, &mon), HU_OK);
    HU_ASSERT_NOT_NULL(mon);
    hu_channel_monitor_destroy(mon);
}

static void test_create_null_args(void) {
    hu_channel_monitor_t *mon = NULL;
    HU_ASSERT_EQ(hu_channel_monitor_create(NULL, NULL, &mon), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_channel_monitor_create(&s_alloc, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_add_channel(void) {
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    HU_ASSERT_EQ(hu_channel_monitor_add(mon, &s_mock_channel), HU_OK);

    const hu_channel_status_t *status = NULL;
    size_t count = 0;
    hu_channel_monitor_get_status(mon, &status, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_STR_EQ(status[0].channel_name, "mock-channel");
    HU_ASSERT(status[0].healthy == true);
    hu_channel_monitor_destroy(mon);
}

static void test_tick_healthy(void) {
    s_mock_healthy = true;
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    cfg.check_interval_sec = 0;
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    hu_channel_monitor_add(mon, &s_mock_channel);

    HU_ASSERT_EQ(hu_channel_monitor_tick(mon, 100), HU_OK);

    const hu_channel_status_t *status = NULL;
    size_t count = 0;
    hu_channel_monitor_get_status(mon, &status, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT(status[0].healthy == true);
    HU_ASSERT_EQ(status[0].last_healthy_ts, 100);
    HU_ASSERT_EQ(status[0].consecutive_failures, 0);
    hu_channel_monitor_destroy(mon);
}

static void test_tick_unhealthy_tracks_failures(void) {
    s_mock_healthy = false;
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    cfg.check_interval_sec = 0;
    cfg.max_restart_count = 3;
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    hu_channel_monitor_add(mon, &s_mock_channel);

    hu_channel_monitor_tick(mon, 100);

    const hu_channel_status_t *status = NULL;
    size_t count = 0;
    hu_channel_monitor_get_status(mon, &status, &count);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT(status[0].healthy == false);
    HU_ASSERT_EQ(status[0].consecutive_failures, 1);
    HU_ASSERT_EQ(status[0].restart_count, 1);
    HU_ASSERT_GT(strlen(status[0].last_error), 0);
    hu_channel_monitor_destroy(mon);
}

static void test_backoff_doubles(void) {
    s_mock_healthy = false;
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    cfg.check_interval_sec = 0;
    cfg.backoff_initial_sec = 2;
    cfg.backoff_max_sec = 16;
    cfg.max_restart_count = 10;
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    hu_channel_monitor_add(mon, &s_mock_channel);

    hu_channel_monitor_tick(mon, 100);
    const hu_channel_status_t *st = NULL;
    size_t cnt = 0;
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT_EQ(st[0].current_backoff_sec, 4);

    hu_channel_monitor_tick(mon, 200);
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT_EQ(st[0].current_backoff_sec, 8);

    hu_channel_monitor_tick(mon, 400);
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT_EQ(st[0].current_backoff_sec, 16);

    /* Cap at max */
    hu_channel_monitor_tick(mon, 600);
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT_EQ(st[0].current_backoff_sec, 16);

    hu_channel_monitor_destroy(mon);
}

static void test_record_event(void) {
    s_mock_healthy = true;
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    cfg.check_interval_sec = 0;
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    hu_channel_monitor_add(mon, &s_mock_channel);

    hu_channel_monitor_tick(mon, 100);
    hu_channel_monitor_record_event(mon, "mock-channel");

    const hu_channel_status_t *st = NULL;
    size_t cnt = 0;
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT_EQ(st[0].last_event_ts, 100);
    hu_channel_monitor_destroy(mon);
}

static void test_stale_event_warning(void) {
    s_mock_healthy = true;
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    cfg.check_interval_sec = 0;
    cfg.stale_event_threshold = 60;
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    hu_channel_monitor_add(mon, &s_mock_channel);

    hu_channel_monitor_tick(mon, 100);
    hu_channel_monitor_record_event(mon, "mock-channel");

    /* Tick way after stale threshold */
    hu_channel_monitor_tick(mon, 200);
    const hu_channel_status_t *st = NULL;
    size_t cnt = 0;
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT(st[0].healthy == true);
    HU_ASSERT_STR_CONTAINS(st[0].last_error, "no events");
    hu_channel_monitor_destroy(mon);
}

static void test_recovery_resets_state(void) {
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    cfg.check_interval_sec = 0;
    cfg.backoff_initial_sec = 2;
    cfg.max_restart_count = 5;
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    hu_channel_monitor_add(mon, &s_mock_channel);

    s_mock_healthy = false;
    hu_channel_monitor_tick(mon, 100);

    const hu_channel_status_t *st = NULL;
    size_t cnt = 0;
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT_EQ(st[0].consecutive_failures, 1);
    HU_ASSERT_EQ(st[0].current_backoff_sec, 4);

    s_mock_healthy = true;
    hu_channel_monitor_tick(mon, 200);
    hu_channel_monitor_get_status(mon, &st, &cnt);
    HU_ASSERT(st[0].healthy == true);
    HU_ASSERT_EQ(st[0].consecutive_failures, 0);
    HU_ASSERT_EQ(st[0].current_backoff_sec, 2);
    hu_channel_monitor_destroy(mon);
}

static void test_default_config_values(void) {
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    HU_ASSERT_EQ(cfg.check_interval_sec, 30);
    HU_ASSERT_EQ(cfg.max_restart_count, 5);
    HU_ASSERT_EQ(cfg.backoff_initial_sec, 2);
    HU_ASSERT_EQ(cfg.backoff_max_sec, 120);
    HU_ASSERT_EQ(cfg.stale_event_threshold, 300);
}

static void test_add_null_args(void) {
    hu_channel_monitor_t *mon = NULL;
    hu_channel_monitor_config_t cfg = hu_channel_monitor_config_default();
    hu_channel_monitor_create(&s_alloc, &cfg, &mon);
    HU_ASSERT_EQ(hu_channel_monitor_add(NULL, &s_mock_channel), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_channel_monitor_add(mon, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_channel_monitor_destroy(mon);
}

static void test_tick_null(void) {
    HU_ASSERT_EQ(hu_channel_monitor_tick(NULL, 100), HU_ERR_INVALID_ARGUMENT);
}

void run_channel_monitor_tests(void) {
    HU_TEST_SUITE("channel_monitor");
    HU_RUN_TEST(test_create_destroy);
    HU_RUN_TEST(test_create_null_args);
    HU_RUN_TEST(test_add_channel);
    HU_RUN_TEST(test_tick_healthy);
    HU_RUN_TEST(test_tick_unhealthy_tracks_failures);
    HU_RUN_TEST(test_backoff_doubles);
    HU_RUN_TEST(test_record_event);
    HU_RUN_TEST(test_stale_event_warning);
    HU_RUN_TEST(test_recovery_resets_state);
    HU_RUN_TEST(test_default_config_values);
    HU_RUN_TEST(test_add_null_args);
    HU_RUN_TEST(test_tick_null);
}
