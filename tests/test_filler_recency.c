#include "human/filler_recency.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void make_chat_id(char *buf, size_t buf_sz, int n) {
    snprintf(buf, buf_sz, "chat%d", n);
}

/* ── basic record / lookup ───────────────────────────────────────────────── */

static void record_and_last_returns_recorded_index(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_filler_recency_record(&r, "chat1", 5, 7);

    int32_t got = hu_filler_recency_last(&r, "chat1", 5);
    HU_ASSERT_EQ(got, 7);
}

static void update_existing_chat_overwrites_index(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_filler_recency_record(&r, "chat1", 5, 3);
    hu_filler_recency_record(&r, "chat1", 5, 11);

    HU_ASSERT_EQ(hu_filler_recency_last(&r, "chat1", 5), 11);
}

static void multiple_chats_independent(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_filler_recency_record(&r, "alpha", 5, 1);
    hu_filler_recency_record(&r, "beta", 4, 2);
    hu_filler_recency_record(&r, "gamma", 5, 3);
    hu_filler_recency_record(&r, "delta", 5, 4);
    hu_filler_recency_record(&r, "eps", 3, 5);

    HU_ASSERT_EQ(hu_filler_recency_last(&r, "alpha", 5), 1);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "beta", 4), 2);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "gamma", 5), 3);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "delta", 5), 4);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "eps", 3), 5);
}

static void unknown_chat_returns_minus_one(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    hu_filler_recency_record(&r, "known", 5, 42);

    HU_ASSERT_EQ(hu_filler_recency_last(&r, "missing", 7), -1);
}

/* ── LRU behaviour ───────────────────────────────────────────────────────── */

static void lru_evicts_oldest_on_overflow(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    /* Insert HU_FILLER_RECENCY_CAPACITY + 1 distinct chat_ids. The first
     * inserted should be evicted because every subsequent record bumps
     * next_seq above its last_used_seq. */
    char chat[32];
    int total = HU_FILLER_RECENCY_CAPACITY + 1;
    for (int i = 0; i < total; ++i) {
        make_chat_id(chat, sizeof(chat), i);
        hu_filler_recency_record(&r, chat, strlen(chat), (uint16_t)i);
    }

    /* chat0 was inserted first and never touched again — it must be gone. */
    make_chat_id(chat, sizeof(chat), 0);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, chat, strlen(chat)), -1);

    /* The most recent insert must still be present. */
    make_chat_id(chat, sizeof(chat), total - 1);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, chat, strlen(chat)), (int32_t)(total - 1));
}

static void lru_eviction_picks_least_recently_used(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    /* Insert A, B, C; then touch A again so B is now the LRU. */
    hu_filler_recency_record(&r, "A", 1, 10);
    hu_filler_recency_record(&r, "B", 1, 20);
    hu_filler_recency_record(&r, "C", 1, 30);
    hu_filler_recency_record(&r, "A", 1, 11); /* updates last_used_seq for A */

    /* Now fill the rest of the table with fresh chats. */
    char chat[32];
    for (int i = 0; i < HU_FILLER_RECENCY_CAPACITY - 3; ++i) {
        make_chat_id(chat, sizeof(chat), 100 + i);
        hu_filler_recency_record(&r, chat, strlen(chat), (uint16_t)(100 + i));
    }

    /* Table is now full. One more insert must evict B (the LRU). */
    hu_filler_recency_record(&r, "Z", 1, 99);

    HU_ASSERT_EQ(hu_filler_recency_last(&r, "B", 1), -1);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "A", 1), 11);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "C", 1), 30);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "Z", 1), 99);
}

/* ── safety / edge cases ─────────────────────────────────────────────────── */

static void null_recency_safe(void) {
    /* Must not crash. */
    hu_filler_recency_record(NULL, "chat1", 5, 7);
    HU_ASSERT_EQ(hu_filler_recency_last(NULL, "chat1", 5), -1);
}

static void null_chat_id_safe(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    /* Must not crash. */
    hu_filler_recency_record(&r, NULL, 0, 7);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, NULL, 0), -1);
}

static void length_mismatch_does_not_collide(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    /* "chat1" (len 5) and "chat10" (len 6) share a prefix.  Storing one
     * must not be returned for a lookup against the other. */
    hu_filler_recency_record(&r, "chat1", 5, 1);
    hu_filler_recency_record(&r, "chat10", 6, 2);

    HU_ASSERT_EQ(hu_filler_recency_last(&r, "chat1", 5), 1);
    HU_ASSERT_EQ(hu_filler_recency_last(&r, "chat10", 6), 2);
}

static void overlong_chat_id_truncates_safely(void) {
    hu_filler_recency_t r;
    memset(&r, 0, sizeof(r));

    char big[300];
    memset(big, 'x', sizeof(big));

    /* Length 300 is well past HU_FILLER_RECENCY_CHAT_ID_MAX (128).  The
     * record path must clamp the copy and not write past the buffer.
     * Subsequent lookup with the same overlong length must succeed via
     * the same truncation. */
    hu_filler_recency_record(&r, big, sizeof(big), 9);

    HU_ASSERT_EQ(hu_filler_recency_last(&r, big, sizeof(big)), 9);
}

/* ── suite registration ──────────────────────────────────────────────────── */

void run_filler_recency_tests(void);

void run_filler_recency_tests(void) {
    HU_TEST_SUITE("Filler Recency LRU");

    HU_RUN_TEST(record_and_last_returns_recorded_index);
    HU_RUN_TEST(update_existing_chat_overwrites_index);
    HU_RUN_TEST(multiple_chats_independent);
    HU_RUN_TEST(unknown_chat_returns_minus_one);
    HU_RUN_TEST(lru_evicts_oldest_on_overflow);
    HU_RUN_TEST(lru_eviction_picks_least_recently_used);
    HU_RUN_TEST(null_recency_safe);
    HU_RUN_TEST(null_chat_id_safe);
    HU_RUN_TEST(length_mismatch_does_not_collide);
    HU_RUN_TEST(overlong_chat_id_truncates_safely);
}
