/* ─────────────────────────────────────────────────────────────────────────
 * test_win_detect.c — pins B1a conservative win detection.
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 *
 * Key invariant: a setback is NEVER a win ("I didn't get the job"), because
 * celebrating a non-win is far worse than missing one.
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/behavior/win_detect.h"
#include "test_framework.h"
#include <string.h>

static hu_win_signal_t detect(const char *s) {
    return hu_win_detect(s, strlen(s));
}

static void win_detects_achievement(void) {
    hu_win_signal_t w = detect("I did it — I passed the bar exam!");
    HU_ASSERT_TRUE(w.is_win);
    HU_ASSERT_EQ((int)w.kind, (int)HU_WIN_ACHIEVEMENT);
    HU_ASSERT_TRUE(w.confidence > 0.5);
}

static void win_detects_milestone(void) {
    hu_win_signal_t w = detect("today is our ten years together");
    HU_ASSERT_TRUE(w.is_win);
    HU_ASSERT_EQ((int)w.kind, (int)HU_WIN_MILESTONE);
}

static void win_detects_good_news(void) {
    hu_win_signal_t w = detect("guess what, I have great news to share");
    HU_ASSERT_TRUE(w.is_win);
    HU_ASSERT_EQ((int)w.kind, (int)HU_WIN_GOOD_NEWS);
}

/* The load-bearing safety case: a setback must NOT be celebrated. */
static void win_negation_is_not_a_win(void) {
    HU_ASSERT_FALSE(detect("I didn't get the job").is_win);
    HU_ASSERT_FALSE(detect("unfortunately I failed the exam").is_win);
    HU_ASSERT_FALSE(detect("the deal fell through").is_win);
    HU_ASSERT_FALSE(detect("I did not pass").is_win);
}

static void win_neutral_is_not_a_win(void) {
    HU_ASSERT_FALSE(detect("what's the weather tomorrow?").is_win);
    HU_ASSERT_FALSE(detect("can you help me debug this?").is_win);
    HU_ASSERT_FALSE(detect("").is_win);
}

static void win_null_safe(void) {
    HU_ASSERT_FALSE(hu_win_detect(NULL, 0).is_win);
}

void run_win_detect_tests(void);
void run_win_detect_tests(void) {
    HU_TEST_SUITE("win_detect");
    HU_RUN_TEST(win_detects_achievement);
    HU_RUN_TEST(win_detects_milestone);
    HU_RUN_TEST(win_detects_good_news);
    HU_RUN_TEST(win_negation_is_not_a_win);
    HU_RUN_TEST(win_neutral_is_not_a_win);
    HU_RUN_TEST(win_null_safe);
}
