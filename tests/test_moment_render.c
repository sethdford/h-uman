#include "human/moment.h"
#include "test_framework.h"

#include <string.h>

/* Common timestamp used across render tests. */
#define TS_TEST ((int64_t)1779609600)

/* Build a moment with a few useful defaults so tests aren't repetitive. */
static hu_moment_t make_mid_thread_moment(void) {
    hu_moment_t m = {0};
    m.source_flags = HU_MOMENT_SRC_HISTORY | HU_MOMENT_SRC_LAST_THEIR_TS;
    m.phase_local = HU_MOMENT_PHASE_NIGHT;
    m.time_since_their_last_msg_s = 3 * 3600;
    m.thread_is_continuation = false;
    m.topic_still_open = true;
    snprintf(m.topic_hint, sizeof m.topic_hint, "%s", "monday standup");
    m.their_avg_length_words = 8;
    m.their_recent_tone = HU_MOMENT_TONE_TERSE;
    m.they_use_lowercase = true;
    m.suggested_open = HU_MOMENT_OPEN_NONE;
    m.suggested_brevity = HU_MOMENT_BREVITY_MIRROR;
    m.composed_at_s = TS_TEST;
    return m;
}

static void render_mid_thread_includes_time_phase_and_brevity_cue(void) {
    hu_moment_t m = make_mid_thread_moment();
    char buf[256];
    size_t n = 0;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, buf, sizeof buf, &n), HU_OK);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "[moment]"));
    HU_ASSERT_NOT_NULL(strstr(buf, "night"));
    HU_ASSERT_NOT_NULL(strstr(buf, "3h"));
    HU_ASSERT_NOT_NULL(strstr(buf, "8 words"));
    HU_ASSERT_NOT_NULL(strstr(buf, "no greeting"));
    HU_ASSERT_NOT_NULL(strstr(buf, "match their length"));
}

static void render_reconnect_includes_reconnect_cue(void) {
    hu_moment_t m = {0};
    m.source_flags = HU_MOMENT_SRC_LAST_THEIR_TS;
    m.phase_local = HU_MOMENT_PHASE_MORNING;
    m.time_since_their_last_msg_s = 4 * 86400;
    m.suggested_open = HU_MOMENT_OPEN_RECONNECT;
    m.suggested_brevity = HU_MOMENT_BREVITY_SHORT;
    char buf[256];
    size_t n = 0;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, buf, sizeof buf, &n), HU_OK);
    HU_ASSERT_NOT_NULL(strstr(buf, "4d"));
    HU_ASSERT_NOT_NULL(strstr(buf, "reconnect"));
}

static void render_greet_morning_includes_morning_cue(void) {
    hu_moment_t m = {0};
    m.source_flags = HU_MOMENT_SRC_LAST_THEIR_TS;
    m.phase_local = HU_MOMENT_PHASE_MORNING;
    m.time_since_their_last_msg_s = 14 * 3600;
    m.suggested_open = HU_MOMENT_OPEN_GREET_MORNING;
    m.suggested_brevity = HU_MOMENT_BREVITY_SHORT;
    char buf[256];
    size_t n = 0;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, buf, sizeof buf, &n), HU_OK);
    HU_ASSERT_NOT_NULL(strstr(buf, "morning"));
    HU_ASSERT_NOT_NULL(strstr(buf, "greet for a fresh morning"));
}

static void render_returns_error_when_cap_too_small(void) {
    hu_moment_t m = make_mid_thread_moment();
    char buf[16];
    size_t n = 0;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, buf, sizeof buf, &n), HU_ERR_INVALID_ARGUMENT);
}

static void render_empty_moment_writes_empty_buffer(void) {
    hu_moment_t m = {0}; /* source_flags = 0 → empty fragment */
    char buf[256];
    size_t n = 99;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, buf, sizeof buf, &n), HU_OK);
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_EQ(buf[0], '\0');
}

static void render_rejects_null_moment(void) {
    char buf[256];
    size_t n;
    HU_ASSERT_EQ(hu_moment_render_prompt(NULL, buf, sizeof buf, &n), HU_ERR_INVALID_ARGUMENT);
}

static void render_rejects_null_buf(void) {
    hu_moment_t m = make_mid_thread_moment();
    size_t n;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, NULL, 256, &n), HU_ERR_INVALID_ARGUMENT);
}

static void render_rejects_zero_cap(void) {
    hu_moment_t m = make_mid_thread_moment();
    char buf[256];
    size_t n;
    HU_ASSERT_EQ(hu_moment_render_prompt(&m, buf, 0, &n), HU_ERR_INVALID_ARGUMENT);
}

void run_moment_render_tests(void) {
    HU_TEST_SUITE("moment_render");
    HU_RUN_TEST(render_mid_thread_includes_time_phase_and_brevity_cue);
    HU_RUN_TEST(render_reconnect_includes_reconnect_cue);
    HU_RUN_TEST(render_greet_morning_includes_morning_cue);
    HU_RUN_TEST(render_returns_error_when_cap_too_small);
    HU_RUN_TEST(render_empty_moment_writes_empty_buffer);
    HU_RUN_TEST(render_rejects_null_moment);
    HU_RUN_TEST(render_rejects_null_buf);
    HU_RUN_TEST(render_rejects_zero_cap);
}
