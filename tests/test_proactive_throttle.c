/*
 * test_proactive_throttle.c — Regression coverage for the 2026-05-16 proactive
 * messaging incident.
 *
 * Pins:
 *   P1-6: Per-channel rate limiter must reject the 2nd+ send within window
 *         (was bypassed entirely on proactive path before this fix).
 *   P1-7: Per-(contact_id, ymd) dedup must scale beyond 8 contacts (the old
 *         ring buffer wrapped silently with 9+ contacts).
 *   P4-6: Per-contact daily (24h) / weekly (7d) cap enforced (defense in
 *         depth — even if rate-limiter and dedup pass).
 */
#include "human/agent/proactive_throttle.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- P1-6: per-channel rate limiter on proactive path ----------- */

static void throttle_channel_rate_limit_blocks_burst(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    /* Twilio default in rate_limit.c is 1 token, 1 token/sec refill — so 4
     * back-to-back tries must accept exactly the first and reject 2..4. This
     * is the failure mode the incident showed ("how'd it go with the loan?"
     * fired 4x to Mindy on the F25 emotional check-in path). */
    int accepted = 0;
    for (int i = 0; i < 4; i++) {
        if (hu_proactive_throttle_channel_try_consume(&t, "twilio"))
            accepted++;
    }
    HU_ASSERT_EQ(accepted, 1);
}

static void throttle_channel_rate_limit_null_channel_allows(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);
    HU_ASSERT_TRUE(hu_proactive_throttle_channel_try_consume(&t, NULL));
    HU_ASSERT_TRUE(hu_proactive_throttle_channel_try_consume(&t, ""));
}

static void throttle_channel_rate_limit_isolated_per_channel(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);
    /* twilio bucket = 1 — first try consumes; slack bucket = 1 — first try
     * consumes. The two buckets must NOT share state. */
    HU_ASSERT_TRUE(hu_proactive_throttle_channel_try_consume(&t, "twilio"));
    HU_ASSERT_TRUE(hu_proactive_throttle_channel_try_consume(&t, "slack"));
    HU_ASSERT_FALSE(hu_proactive_throttle_channel_try_consume(&t, "twilio"));
}

/* ---------- P1-7: heap-backed per-(contact, ymd) dedup ---------------- */

static void throttle_dedup_first_wins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    HU_ASSERT_TRUE(hu_proactive_throttle_dedup_first_today(&t, "gm", "alice", 20260516));
    HU_ASSERT_FALSE(hu_proactive_throttle_dedup_first_today(&t, "gm", "alice", 20260516));
}

static void throttle_dedup_scales_beyond_eight_contacts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    /* The pre-fix ring buffer was [8]; with 9+ contacts the 9th and beyond
     * would silently overflow and re-receive the daily message every cycle.
     * Verify 12 distinct contacts each get exactly one accept then reject. */
    char id[32];
    for (int i = 0; i < 12; i++) {
        snprintf(id, sizeof(id), "contact_%02d", i);
        HU_ASSERT_TRUE(hu_proactive_throttle_dedup_first_today(&t, "gm", id, 20260516));
    }
    for (int i = 0; i < 12; i++) {
        snprintf(id, sizeof(id), "contact_%02d", i);
        HU_ASSERT_FALSE(hu_proactive_throttle_dedup_first_today(&t, "gm", id, 20260516));
    }
}

static void throttle_dedup_rolls_over_on_new_day(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    HU_ASSERT_TRUE(hu_proactive_throttle_dedup_first_today(&t, "gm", "alice", 20260516));
    HU_ASSERT_FALSE(hu_proactive_throttle_dedup_first_today(&t, "gm", "alice", 20260516));
    /* Different ymd — should accept again. */
    HU_ASSERT_TRUE(hu_proactive_throttle_dedup_first_today(&t, "gm", "alice", 20260517));
}

static void throttle_dedup_isolated_per_feature(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    HU_ASSERT_TRUE(hu_proactive_throttle_dedup_first_today(&t, "gm", "alice", 20260516));
    /* Different feature — should accept on same day. */
    HU_ASSERT_TRUE(
        hu_proactive_throttle_dedup_first_today(&t, "important_date", "alice", 20260516));
}

/* ---------- P4-6: per-contact daily/weekly cap ------------------------ */

static void throttle_record_send_first_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    uint64_t now = 1715900000000ULL;
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&t, "mindy", "F25", now));
}

static void throttle_record_send_blocks_second_within_24h(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    uint64_t now = 1715900000000ULL;
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&t, "mindy", "F25", now));
    /* 23h59m later — still inside 24h window. */
    HU_ASSERT_FALSE(hu_proactive_throttle_record_send(
        &t, "mindy", "proactive", now + (23ULL * 3600ULL + 59ULL * 60ULL) * 1000ULL));
}

static void throttle_record_send_blocks_fourth_in_seven_days(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);
    uint64_t base = 1715900000000ULL;
    /* Three sends one day apart (each clears daily cap). */
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&t, "mindy", "F25", base));
    HU_ASSERT_TRUE(
        hu_proactive_throttle_record_send(&t, "mindy", "F30", base + 1ULL * 86400000ULL));
    HU_ASSERT_TRUE(
        hu_proactive_throttle_record_send(&t, "mindy", "F31", base + 2ULL * 86400000ULL));
    /* Fourth within 7 days — weekly cap = 3 (default). */
    HU_ASSERT_FALSE(
        hu_proactive_throttle_record_send(&t, "mindy", "proactive", base + 3ULL * 86400000ULL));
}

static void throttle_record_send_resets_after_window(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);

    uint64_t now = 1715900000000ULL;
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&t, "mindy", "F25", now));
    /* 25h later — the older send is outside the 24h window so daily resets. */
    HU_ASSERT_TRUE(
        hu_proactive_throttle_record_send(&t, "mindy", "F25", now + 25ULL * 3600ULL * 1000ULL));
}

static void throttle_record_send_isolated_per_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);
    uint64_t now = 1715900000000ULL;
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&t, "mindy", "F25", now));
    /* Different contact — should still accept. */
    HU_ASSERT_TRUE(hu_proactive_throttle_record_send(&t, "betty", "F25", now));
    /* Same contact retry — should reject. */
    HU_ASSERT_FALSE(hu_proactive_throttle_record_send(&t, "mindy", "F25", now + 1000));
}

static void throttle_count_in_window_matches_actual(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_proactive_throttle_t t;
    hu_proactive_throttle_init(&t, &alloc);
    uint64_t base = 1715900000000ULL;
    /* Bump caps so we can record 5 sends. */
    t.daily_cap = 100;
    t.weekly_cap = 100;
    for (int i = 0; i < 5; i++)
        HU_ASSERT_TRUE(
            hu_proactive_throttle_record_send(&t, "mindy", "F25", base + (uint64_t)i * 3600000ULL));
    /* All 5 within the last 24h. */
    HU_ASSERT_EQ(hu_proactive_throttle_count_in_window(&t, "mindy", base + 5ULL * 3600000ULL,
                                                       24ULL * 3600000ULL),
                 5u);
    /* Only 2 strictly within the last 3 hours (window is strict `<` so the
     * send at delta=3h sits on the boundary and is excluded). */
    HU_ASSERT_EQ(hu_proactive_throttle_count_in_window(&t, "mindy", base + 5ULL * 3600000ULL,
                                                       3ULL * 3600000ULL),
                 2u);
}

void run_proactive_throttle_tests(void) {
    HU_TEST_SUITE("proactive_throttle");
    HU_RUN_TEST(throttle_channel_rate_limit_blocks_burst);
    HU_RUN_TEST(throttle_channel_rate_limit_null_channel_allows);
    HU_RUN_TEST(throttle_channel_rate_limit_isolated_per_channel);
    HU_RUN_TEST(throttle_dedup_first_wins);
    HU_RUN_TEST(throttle_dedup_scales_beyond_eight_contacts);
    HU_RUN_TEST(throttle_dedup_rolls_over_on_new_day);
    HU_RUN_TEST(throttle_dedup_isolated_per_feature);
    HU_RUN_TEST(throttle_record_send_first_succeeds);
    HU_RUN_TEST(throttle_record_send_blocks_second_within_24h);
    HU_RUN_TEST(throttle_record_send_blocks_fourth_in_seven_days);
    HU_RUN_TEST(throttle_record_send_resets_after_window);
    HU_RUN_TEST(throttle_record_send_isolated_per_contact);
    HU_RUN_TEST(throttle_count_in_window_matches_actual);
}
