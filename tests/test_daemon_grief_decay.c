/* test_daemon_grief_decay.c — DEF-10: one heavy message must not silence
 * check-ins forever. HU_GRIEF_DECAY ends the suppression after a quiet window. */
#include "human/daemon/grief_decay.h"

#include "human/daemon/proposer_context.h"
#include "human/provider.h"
#include "test_framework.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char k_grief[] = "i lost my dad last night";
static const char k_light[] = "lol that meme was good";

/* A proposer context the way the daemon builds one for a check-in. */
static hu_proposer_context_t ctx_live(void) {
    static const hu_provider_vtable_t vt = {0};
    hu_provider_t local = {.ctx = (void *)&vt, .vtable = &vt};
    hu_proposer_context_t pc;
    hu_proposer_context_begin_with_local(&pc, HU_GATE_LIVE, &local);
    return pc;
}

static bool suppressed(const hu_grief_decay_inbound_t *li, int64_t now) {
    hu_proposer_context_t pc = ctx_live();
    return hu_grief_decay_decide(li, now, &pc) == HU_GRIEF_DECAY_SUPPRESS;
}

static void clear_env(void) {
    unsetenv("HU_GRIEF_DECAY");
    unsetenv("HU_GRIEF_DECAY_QUIET_HOURS");
}

/* A "%Y-%m-%d %H:%M" local stamp `hours_ago` before now. */
static void stamp(char *buf, size_t cap, time_t now, double hours_ago) {
    time_t t = now - (time_t)(hours_ago * 3600.0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tmv);
}

static hu_grief_decay_inbound_t inbound(const char *text, time_t now, double hours_ago) {
    hu_grief_decay_inbound_t li;
    memset(&li, 0, sizeof(li));
    char ts[32];
    stamp(ts, sizeof(ts), now, hours_ago);
    hu_grief_decay_note_inbound(&li, text, strlen(text), ts);
    return li;
}

static void test_mode_defaults_off_and_parses(void) {
    clear_env();
    HU_ASSERT_EQ((int)hu_grief_decay_mode(), (int)HU_GATE_OFF);
    setenv("HU_GRIEF_DECAY", "shadow", 1);
    HU_ASSERT_EQ((int)hu_grief_decay_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_GRIEF_DECAY", "live", 1);
    HU_ASSERT_EQ((int)hu_grief_decay_mode(), (int)HU_GATE_LIVE);
    setenv("HU_GRIEF_DECAY", "nope", 1);
    HU_ASSERT_EQ((int)hu_grief_decay_mode(), (int)HU_GATE_OFF);
    clear_env();
}

static void test_quiet_hours_default_override_and_clamp(void) {
    clear_env();
    HU_ASSERT_TRUE(fabs(hu_grief_decay_quiet_hours() - HU_GRIEF_DECAY_DEFAULT_QUIET_HOURS) < 1e-9);
    setenv("HU_GRIEF_DECAY_QUIET_HOURS", "6", 1);
    HU_ASSERT_TRUE(fabs(hu_grief_decay_quiet_hours() - 6.0) < 1e-9);
    setenv("HU_GRIEF_DECAY_QUIET_HOURS", "0.1", 1);
    HU_ASSERT_TRUE(fabs(hu_grief_decay_quiet_hours() - HU_GRIEF_DECAY_MIN_QUIET_HOURS) < 1e-9);
    setenv("HU_GRIEF_DECAY_QUIET_HOURS", "9999", 1);
    HU_ASSERT_TRUE(fabs(hu_grief_decay_quiet_hours() - HU_GRIEF_DECAY_MAX_QUIET_HOURS) < 1e-9);
    setenv("HU_GRIEF_DECAY_QUIET_HOURS", "soon", 1);
    HU_ASSERT_TRUE(fabs(hu_grief_decay_quiet_hours() - HU_GRIEF_DECAY_DEFAULT_QUIET_HOURS) < 1e-9);
    clear_env();
}

static void test_parse_ts_round_trips_local_minutes(void) {
    time_t now = time(NULL);
    char ts[32];
    stamp(ts, sizeof(ts), now, 3.0);
    int64_t t = hu_grief_decay_parse_ts(ts);
    HU_ASSERT_TRUE(t > 0);
    HU_ASSERT_TRUE(llabs((long long)((int64_t)now - 3 * 3600 - t)) <= 60);
    HU_ASSERT_EQ(hu_grief_decay_parse_ts("not a time"), (int64_t)-1);
    HU_ASSERT_EQ(hu_grief_decay_parse_ts(""), (int64_t)-1);
    HU_ASSERT_EQ(hu_grief_decay_parse_ts(NULL), (int64_t)-1);
}

static void test_quiet_window_is_time_bounded(void) {
    HU_ASSERT_TRUE(hu_grief_decay_in_quiet_window(1000, 1000 + 3600, 24.0));
    HU_ASSERT_FALSE(hu_grief_decay_in_quiet_window(1000, 1000 + 25 * 3600, 24.0));
    HU_ASSERT_TRUE(hu_grief_decay_in_quiet_window(-1, 1000000, 24.0)); /* unknown: quiet */
}

static void test_note_inbound_truncates_and_keeps_ts(void) {
    hu_grief_decay_inbound_t li;
    memset(&li, 0, sizeof(li));
    char big[2000];
    memset(big, 'a', sizeof(big));
    hu_grief_decay_note_inbound(&li, big, sizeof(big), "2026-10-01 09:30");
    HU_ASSERT_EQ(li.len, sizeof(li.text) - 1);
    HU_ASSERT_EQ(li.text[li.len], '\0');
    HU_ASSERT_STR_EQ(li.ts, "2026-10-01 09:30");
    hu_grief_decay_note_inbound(&li, "ok", 2, NULL);
    HU_ASSERT_STR_EQ(li.text, "ok");
    HU_ASSERT_STR_EQ(li.ts, "");
}

/* Headline: three days after a grief message the check-in is suppressed
 * forever under OFF (today) and eligible again under LIVE. */
static void test_live_ends_suppression_after_quiet_window(void) {
    time_t now = time(NULL);
    hu_grief_decay_inbound_t old = inbound(k_grief, now, 72.0);
    clear_env();
    HU_ASSERT_TRUE(suppressed(&old, (int64_t)now)); /* off: today */
    setenv("HU_GRIEF_DECAY", "shadow", 1);
    HU_ASSERT_TRUE(suppressed(&old, (int64_t)now)); /* shadow: unchanged */
    setenv("HU_GRIEF_DECAY", "live", 1);
    HU_ASSERT_FALSE(suppressed(&old, (int64_t)now)); /* live: eligible */
    clear_env();
}

static void test_live_keeps_quiet_inside_window_and_on_unknown_time(void) {
    time_t now = time(NULL);
    setenv("HU_GRIEF_DECAY", "live", 1);
    hu_grief_decay_inbound_t recent = inbound(k_grief, now, 2.0);
    HU_ASSERT_TRUE(suppressed(&recent, (int64_t)now));
    hu_grief_decay_inbound_t no_ts;
    memset(&no_ts, 0, sizeof(no_ts));
    hu_grief_decay_note_inbound(&no_ts, k_grief, strlen(k_grief), "garbage");
    HU_ASSERT_TRUE(suppressed(&no_ts, (int64_t)now));
    /* the window is the env value: 1 h makes the 2 h-old message eligible */
    setenv("HU_GRIEF_DECAY_QUIET_HOURS", "1", 1);
    HU_ASSERT_FALSE(suppressed(&recent, (int64_t)now));
    clear_env();
}

/* LIVE needs the proposer to see the thread: without HU_PROPOSER_CONTEXT live
 * (and a local provider) the old suppression holds. With it, the verdict is
 * GENTLE: extras skipped, the age signal set on the context, no text. */
static void test_live_requires_proposer_context_and_marks_gentle(void) {
    time_t now = time(NULL);
    hu_grief_decay_inbound_t old = inbound(k_grief, now, 72.0);
    setenv("HU_GRIEF_DECAY", "live", 1);
    HU_ASSERT_EQ((int)hu_grief_decay_decide(&old, (int64_t)now, NULL),
                 (int)HU_GRIEF_DECAY_SUPPRESS);
    hu_proposer_context_t off;
    hu_proposer_context_begin_with_local(&off, HU_GATE_SHADOW, NULL);
    HU_ASSERT_EQ((int)hu_grief_decay_decide(&old, (int64_t)now, &off),
                 (int)HU_GRIEF_DECAY_SUPPRESS);
    hu_proposer_context_t pc = ctx_live();
    HU_ASSERT_EQ(pc.heavy_inbound_hours_ago, (int64_t)-1);
    hu_grief_decay_verdict_t v = hu_grief_decay_decide(&old, (int64_t)now, &pc);
    HU_ASSERT_EQ((int)v, (int)HU_GRIEF_DECAY_GENTLE);
    HU_ASSERT_TRUE(hu_grief_decay_skip_extras(v));
    HU_ASSERT_FALSE(hu_grief_decay_skip_extras(HU_GRIEF_DECAY_NONE));
    HU_ASSERT_TRUE(pc.heavy_inbound_hours_ago >= 71 && pc.heavy_inbound_hours_ago <= 72);
    /* the signal reaches the proposer block, as hours only */
    hu_proposer_context_build(&pc, NULL, NULL, NULL, NULL);
    HU_ASSERT_TRUE(strstr(pc.block, "emotionally heavy, about 7") != NULL);
    HU_ASSERT_TRUE(strstr(pc.block, "dad") == NULL);
    clear_env();
}

static void test_light_inbound_never_suppresses(void) {
    time_t now = time(NULL);
    hu_grief_decay_inbound_t light = inbound(k_light, now, 0.5);
    hu_grief_decay_inbound_t empty;
    memset(&empty, 0, sizeof(empty));
    const char *modes[] = {"off", "shadow", "live"};
    for (int i = 0; i < 3; i++) {
        setenv("HU_GRIEF_DECAY", modes[i], 1);
        HU_ASSERT_FALSE(suppressed(&light, (int64_t)now));
        HU_ASSERT_FALSE(suppressed(&empty, (int64_t)now));
    }
    HU_ASSERT_FALSE(suppressed(NULL, (int64_t)now));
    hu_proposer_context_t pc = ctx_live();
    HU_ASSERT_EQ((int)hu_grief_decay_decide(&light, (int64_t)now, &pc), (int)HU_GRIEF_DECAY_NONE);
    clear_env();
}

void run_daemon_grief_decay_tests(void) {
    HU_TEST_SUITE("daemon_grief_decay");
    HU_RUN_TEST(test_mode_defaults_off_and_parses);
    HU_RUN_TEST(test_quiet_hours_default_override_and_clamp);
    HU_RUN_TEST(test_parse_ts_round_trips_local_minutes);
    HU_RUN_TEST(test_quiet_window_is_time_bounded);
    HU_RUN_TEST(test_note_inbound_truncates_and_keeps_ts);
    HU_RUN_TEST(test_live_ends_suppression_after_quiet_window);
    HU_RUN_TEST(test_live_keeps_quiet_inside_window_and_on_unknown_time);
    HU_RUN_TEST(test_live_requires_proposer_context_and_marks_gentle);
    HU_RUN_TEST(test_light_inbound_never_suppresses);
}
