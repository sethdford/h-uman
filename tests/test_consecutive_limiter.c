/* Per-contact consecutive-reply limiter — burst semantics end to end.
 *
 * 2026-09-04: a rental agent sent four messages (two direct questions) 52
 * minutes after the daemon's third output and every one was silenced by a
 * hard-coded cap of 3 that only the real user typing could reset. The
 * limiter now takes its cap from config, expires a burst after a quiet
 * window, and reports the count so the daemon can log a silenced question
 * loudly. The clock is a parameter, so every branch here is deterministic. */
#include "human/daemon/consecutive_limiter.h"
#include "test_framework.h"
#include <string.h>

#define KEY     "+15550000001"
#define KEY_LEN (sizeof(KEY) - 1)

static void limiter_unknown_contact_is_never_silenced(void) {
    hu_consec_limiter_t l;
    hu_consec_limiter_init(&l);
    uint32_t n = 99;
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 3, 1800, 1000, &n));
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_EQ((int)hu_consec_limiter_count(&l, KEY, KEY_LEN), 0);
}

static void limiter_silences_at_the_configured_cap(void) {
    hu_consec_limiter_t l;
    hu_consec_limiter_init(&l);
    for (int i = 0; i < 4; i++)
        hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, 1000 + i);
    uint32_t n = 0;
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 5, 1800, 1010, &n));
    HU_ASSERT_EQ((int)n, 4);
    hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, 1010);
    HU_ASSERT_TRUE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 5, 1800, 1020, &n));
    HU_ASSERT_EQ((int)n, 5);
    /* cap 0 = no cap, whatever the count */
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 0, 1800, 1020, &n));
}

static void limiter_reset_when_the_real_user_steps_in(void) {
    hu_consec_limiter_t l;
    hu_consec_limiter_init(&l);
    for (int i = 0; i < 5; i++)
        hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, 1000 + i);
    HU_ASSERT_TRUE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 5, 1800, 1010, NULL));
    hu_consec_limiter_reset(&l, KEY, KEY_LEN);
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 5, 1800, 1010, NULL));
    HU_ASSERT_EQ((int)hu_consec_limiter_count(&l, KEY, KEY_LEN), 0);
}

/* The 2026-09-04 conversation: three outputs, then the contact returns 52
 * minutes later with questions. With a 30-minute reset window the burst has
 * expired and the questions are answered; the old limiter stayed silent. */
static void limiter_burst_expires_after_the_quiet_window(void) {
    hu_consec_limiter_t l;
    hu_consec_limiter_init(&l);
    const int64_t t0 = 1000;
    hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, t0);        /* 17:48 tapback */
    hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, t0 + 960);  /* 18:04 "good call" */
    hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, t0 + 2280); /* 18:26 song link */
    uint32_t n = 0;
    /* Old cap of 3 with no expiry: silenced. */
    HU_ASSERT_TRUE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 3, 0, t0 + 5400, &n));
    HU_ASSERT_EQ((int)n, 3);
    /* Same moment, 30-minute window (52 minutes since the song): a new burst. */
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 3, 1800, t0 + 5400, &n));
    HU_ASSERT_EQ((int)n, 0);
    /* Replies in the new burst count from one again. */
    hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, t0 + 5410);
    HU_ASSERT_EQ((int)hu_consec_limiter_count(&l, KEY, KEY_LEN), 1);
}

static void limiter_window_edge_is_inclusive_and_zero_disables_expiry(void) {
    hu_consec_limiter_t l;
    hu_consec_limiter_init(&l);
    for (int i = 0; i < 3; i++)
        hu_consec_limiter_note_reply(&l, KEY, KEY_LEN, 1000);
    /* exactly the window later: still the same burst */
    HU_ASSERT_TRUE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 3, 1800, 2800, NULL));
    /* reset window 0: never expires */
    HU_ASSERT_TRUE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 3, 0, 999999, NULL));
    /* one second past the window: new burst */
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, KEY, KEY_LEN, 3, 1800, 2801, NULL));
}

static void limiter_full_table_and_bad_keys_are_safe(void) {
    hu_consec_limiter_t l;
    hu_consec_limiter_init(&l);
    char key[32];
    for (int i = 0; i < HU_CONSEC_MAX_CONTACTS; i++) {
        int n = snprintf(key, sizeof(key), "+1555%08d", i);
        for (int r = 0; r < 5; r++)
            hu_consec_limiter_note_reply(&l, key, (size_t)n, 1000);
    }
    HU_ASSERT_EQ((int)l.used, HU_CONSEC_MAX_CONTACTS);
    /* 33rd contact: never registered, never limited — the pre-rewrite contract */
    const char *extra = "+15559999999";
    for (int r = 0; r < 5; r++)
        hu_consec_limiter_note_reply(&l, extra, strlen(extra), 1000);
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, extra, strlen(extra), 3, 0, 1000, NULL));
    HU_ASSERT_EQ((int)l.used, HU_CONSEC_MAX_CONTACTS);
    /* keys that do not fit, empty keys, NULLs: no crash, no effect */
    char big[HU_CONSEC_KEY_MAX + 8];
    memset(big, 'x', sizeof(big));
    hu_consec_limiter_note_reply(&l, big, sizeof(big), 1000);
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(&l, big, sizeof(big), 1, 0, 1000, NULL));
    hu_consec_limiter_note_reply(&l, KEY, 0, 1000);
    hu_consec_limiter_note_reply(NULL, KEY, KEY_LEN, 1000);
    hu_consec_limiter_reset(NULL, KEY, KEY_LEN);
    HU_ASSERT_FALSE(hu_consec_limiter_should_silence(NULL, KEY, KEY_LEN, 1, 0, 1000, NULL));
    HU_ASSERT_EQ((int)hu_consec_limiter_count(&l, NULL, 3), 0);
}

void run_consecutive_limiter_tests(void) {
    HU_TEST_SUITE("Consecutive Reply Limiter");
    HU_RUN_TEST(limiter_unknown_contact_is_never_silenced);
    HU_RUN_TEST(limiter_silences_at_the_configured_cap);
    HU_RUN_TEST(limiter_reset_when_the_real_user_steps_in);
    HU_RUN_TEST(limiter_burst_expires_after_the_quiet_window);
    HU_RUN_TEST(limiter_window_edge_is_inclusive_and_zero_disables_expiry);
    HU_RUN_TEST(limiter_full_table_and_bad_keys_are_safe);
}
