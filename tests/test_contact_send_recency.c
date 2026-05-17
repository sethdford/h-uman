#include "human/contact_send_recency.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

/* ── basic record / lookup ───────────────────────────────────────────────── */

static void record_and_last_ts_returns_recorded_timestamp(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, "+15551234567", 12, 1234567890LL, HU_SEND_PATH_REACTIVE);

    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "+15551234567", 12), 1234567890LL);
    HU_ASSERT_EQ((int)hu_contact_send_recency_last_path(&r, "+15551234567", 12),
                 (int)HU_SEND_PATH_REACTIVE);
}

static void update_existing_chat_overwrites(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, "alice", 5, 100, HU_SEND_PATH_PROACTIVE);
    hu_contact_send_recency_record(&r, "alice", 5, 200, HU_SEND_PATH_REACTIVE);

    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "alice", 5), 200);
    HU_ASSERT_EQ((int)hu_contact_send_recency_last_path(&r, "alice", 5),
                 (int)HU_SEND_PATH_REACTIVE);
}

static void unknown_contact_returns_zero(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "nobody", 6), 0);
    HU_ASSERT_EQ((int)hu_contact_send_recency_last_path(&r, "nobody", 6), (int)HU_SEND_PATH_NONE);
}

static void multiple_contacts_independent(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, "alice", 5, 100, HU_SEND_PATH_REACTIVE);
    hu_contact_send_recency_record(&r, "bob", 3, 200, HU_SEND_PATH_PROACTIVE);

    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "alice", 5), 100);
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "bob", 3), 200);
    HU_ASSERT_EQ((int)hu_contact_send_recency_last_path(&r, "alice", 5),
                 (int)HU_SEND_PATH_REACTIVE);
    HU_ASSERT_EQ((int)hu_contact_send_recency_last_path(&r, "bob", 3), (int)HU_SEND_PATH_PROACTIVE);
}

/* ── reactive_within predicate ───────────────────────────────────────────── */

static void reactive_within_returns_true_inside_window(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, "mindy", 5, 1000, HU_SEND_PATH_REACTIVE);

    /* 30s after the reactive send, with a 60s window — should be true. */
    HU_ASSERT_TRUE(hu_contact_send_recency_reactive_within(&r, "mindy", 5, 1030, 60));
}

static void reactive_within_returns_false_outside_window(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, "mindy", 5, 1000, HU_SEND_PATH_REACTIVE);

    /* 120s after the reactive send, with a 60s window — should be false. */
    HU_ASSERT_FALSE(hu_contact_send_recency_reactive_within(&r, "mindy", 5, 1120, 60));
}

static void reactive_within_returns_false_for_proactive_record(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    /* Record a PROACTIVE send — the predicate is "reactive within"; a proactive
     * record alone should not satisfy it, even within the window. */
    hu_contact_send_recency_record(&r, "mindy", 5, 1000, HU_SEND_PATH_PROACTIVE);

    HU_ASSERT_FALSE(hu_contact_send_recency_reactive_within(&r, "mindy", 5, 1010, 60));
}

static void reactive_within_returns_false_for_unknown_contact(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, "alice", 5, 1000, HU_SEND_PATH_REACTIVE);

    HU_ASSERT_FALSE(hu_contact_send_recency_reactive_within(&r, "bob", 3, 1010, 60));
}

/* ── eviction ────────────────────────────────────────────────────────────── */

static void lru_evicts_oldest_on_overflow(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    /* Fill all slots with distinct contacts. */
    char id[16];
    for (int i = 0; i < HU_CONTACT_SEND_RECENCY_CAPACITY; ++i) {
        snprintf(id, sizeof(id), "c%d", i);
        hu_contact_send_recency_record(&r, id, strlen(id), 100 + i, HU_SEND_PATH_REACTIVE);
    }

    /* Confirm slot 0 ("c0") is present. */
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "c0", 2), 100);

    /* Insert one more — should evict the LRU (c0). */
    hu_contact_send_recency_record(&r, "newcomer", 8, 9999, HU_SEND_PATH_PROACTIVE);

    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "c0", 2), 0);          /* evicted */
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, "newcomer", 8), 9999); /* present */
}

/* ── null / boundary safety ──────────────────────────────────────────────── */

static void null_recency_is_safe(void) {
    /* Must not crash. */
    hu_contact_send_recency_record(NULL, "x", 1, 1, HU_SEND_PATH_REACTIVE);
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(NULL, "x", 1), 0);
    HU_ASSERT_EQ((int)hu_contact_send_recency_last_path(NULL, "x", 1), (int)HU_SEND_PATH_NONE);
    HU_ASSERT_FALSE(hu_contact_send_recency_reactive_within(NULL, "x", 1, 0, 60));
}

static void null_chat_id_is_safe(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_contact_send_recency_record(&r, NULL, 0, 1, HU_SEND_PATH_REACTIVE);
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, NULL, 0), 0);
    HU_ASSERT_FALSE(hu_contact_send_recency_reactive_within(&r, NULL, 0, 0, 60));
}

static void overlong_chat_id_truncates_safely(void) {
    hu_contact_send_recency_t r;
    memset(&r, 0, sizeof(r));

    /* Pass a chat_id_len well above the cap; should clamp rather than overrun. */
    char long_id[HU_CONTACT_SEND_RECENCY_CHAT_ID_MAX * 2];
    memset(long_id, 'x', sizeof(long_id));
    hu_contact_send_recency_record(&r, long_id, sizeof(long_id), 42, HU_SEND_PATH_REACTIVE);

    /* Look up with the same overlong length — should match the same truncated slot. */
    HU_ASSERT_EQ(hu_contact_send_recency_last_ts(&r, long_id, sizeof(long_id)), 42);
}

/* ── runner ──────────────────────────────────────────────────────────────── */

void run_contact_send_recency_tests(void);

void run_contact_send_recency_tests(void) {
    HU_TEST_SUITE("Contact Send Recency LRU");

    HU_RUN_TEST(record_and_last_ts_returns_recorded_timestamp);
    HU_RUN_TEST(update_existing_chat_overwrites);
    HU_RUN_TEST(unknown_contact_returns_zero);
    HU_RUN_TEST(multiple_contacts_independent);
    HU_RUN_TEST(reactive_within_returns_true_inside_window);
    HU_RUN_TEST(reactive_within_returns_false_outside_window);
    HU_RUN_TEST(reactive_within_returns_false_for_proactive_record);
    HU_RUN_TEST(reactive_within_returns_false_for_unknown_contact);
    HU_RUN_TEST(lru_evicts_oldest_on_overflow);
    HU_RUN_TEST(null_recency_is_safe);
    HU_RUN_TEST(null_chat_id_is_safe);
    HU_RUN_TEST(overlong_chat_id_truncates_safely);
}
