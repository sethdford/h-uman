/* tests/test_log_format.c — the stderr fallback carries a timestamp and a
 * level, and $HU_LOG_LEVEL filters. Pure functions only (no stderr capture). */
#include "human/core/log.h"
#include "test_framework.h"
#include <string.h>

static void test_format_line_has_timestamp_level_component(void) {
    char line[256];
    /* 2026-09-02T02:57:16 local for whatever TZ the runner has: only assert shape. */
    int n = hu_log_format_line(line, sizeof(line), HU_LOG_LEVEL_WARN, "imessage",
                               "iMessage health OK -> STALLED", 0);
    HU_ASSERT_GT(n, 0);
    HU_ASSERT_STR_EQ(line, "0000-00-00T00:00:00 WARN  [imessage] iMessage health OK -> STALLED\n");
    n = hu_log_format_line(line, sizeof(line), HU_LOG_LEVEL_ERROR, "daemon", "boom", 1788350400);
    HU_ASSERT_GT(n, 0);
    /* real time renders as YYYY-MM-DDTHH:MM:SS (19 chars) then " ERROR [daemon] boom" */
    HU_ASSERT_EQ((int)line[4], (int)'-');
    HU_ASSERT_EQ((int)line[10], (int)'T');
    HU_ASSERT_STR_CONTAINS(line, " ERROR [daemon] boom\n");
    n = hu_log_format_line(line, sizeof(line), HU_LOG_LEVEL_INFO, NULL, NULL, 0);
    HU_ASSERT_STR_EQ(line, "0000-00-00T00:00:00 INFO  [?] \n");
}

static void test_level_parse_and_threshold(void) {
    HU_ASSERT_EQ(hu_log_level_parse("error"), HU_LOG_LEVEL_ERROR);
    HU_ASSERT_EQ(hu_log_level_parse("WARN"), HU_LOG_LEVEL_WARN);
    HU_ASSERT_EQ(hu_log_level_parse("warning"), HU_LOG_LEVEL_WARN);
    HU_ASSERT_EQ(hu_log_level_parse("info"), HU_LOG_LEVEL_INFO);
    HU_ASSERT_EQ(hu_log_level_parse(NULL), HU_LOG_LEVEL_INFO);
    HU_ASSERT_EQ(hu_log_level_parse("garbage"), HU_LOG_LEVEL_INFO);

    hu_log_level_set_for_test(HU_LOG_LEVEL_WARN);
    HU_ASSERT_TRUE(hu_log_level_enabled(HU_LOG_LEVEL_ERROR));
    HU_ASSERT_TRUE(hu_log_level_enabled(HU_LOG_LEVEL_WARN));
    HU_ASSERT_FALSE(hu_log_level_enabled(HU_LOG_LEVEL_INFO));
    hu_log_level_set_for_test(HU_LOG_LEVEL_ERROR);
    HU_ASSERT_FALSE(hu_log_level_enabled(HU_LOG_LEVEL_WARN));
    hu_log_level_set_for_test(-1); /* re-arm env read for other suites */
}

typedef struct {
    int count;
    char last[256];
} cap_t;
static void cap_record(void *ctx, const hu_observer_event_t *ev) {
    cap_t *c = (cap_t *)ctx;
    c->count++;
    if (ev && ev->tag == HU_OBSERVER_EVENT_ERR && ev->data.err.message)
        snprintf(c->last, sizeof(c->last), "%s", ev->data.err.message);
}
static const hu_observer_vtable_t cap_vt = {.record_event = cap_record};

static void test_threshold_filters_observer_path_too(void) {
    cap_t cap = {0};
    hu_observer_t obs = {.ctx = &cap, .vtable = &cap_vt};
    hu_log_level_set_for_test(HU_LOG_LEVEL_WARN);
    hu_log_info("t", &obs, "dropped %d", 1);
    HU_ASSERT_EQ(cap.count, 0);
    hu_log_warn("t", &obs, "kept %d", 2);
    HU_ASSERT_EQ(cap.count, 1);
    HU_ASSERT_STR_EQ(cap.last, "kept 2");
    hu_log_error("t", &obs, "kept %d", 3);
    HU_ASSERT_EQ(cap.count, 2);
    hu_log_level_set_for_test(-1);
}

void run_log_format_tests(void) {
    HU_TEST_SUITE("log_format");
    HU_RUN_TEST(test_format_line_has_timestamp_level_component);
    HU_RUN_TEST(test_level_parse_and_threshold);
    HU_RUN_TEST(test_threshold_filters_observer_path_too);
}
