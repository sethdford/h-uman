/* Spec 2026-05-19 self-model-scaffold — Phase A unit tests.
 *
 * Pins:
 *   AC-SM-1: per-turn behavior log records the documented fields and
 *            head advances by exactly one per record.
 *   AC-SM-6: zero-cost when HU_ENABLE_SELF_MODEL is OFF — record() is a
 *            harmless no-op against an uninitialized log.
 *
 * Ring-buffer correctness:
 *   - init zeroes the struct
 *   - record advances head monotonically
 *   - wrap at capacity preserves the most recent N records
 *   - snapshot returns the most recent N records in chronological order
 *   - record performs zero allocations on the hot path (verified via
 *     the project's hu_tracking_allocator_t)
 */

#include "human/agent/self_model.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SELF_MODEL

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static hu_agent_behavior_record_t make_record(uint32_t length_chars, uint32_t tool_seq_hash,
                                              int64_t ts_ms) {
    hu_agent_behavior_record_t r;
    memset(&r, 0, sizeof(r));
    r.response_length_chars = length_chars;
    r.response_length_tokens_est = length_chars / 4;
    r.tool_sequence_hash = tool_seq_hash;
    r.tool_count = 2;
    r.emotional_register = (uint8_t)HU_AGENT_EMOTION_POSITIVE;
    r.persona_delta_kind = (uint8_t)HU_AGENT_PERSONA_DELTA_NONE;
    r.response_latency_ms = 250;
    r.contact_hash = 0xABCD1234DEADBEEFULL;
    r.channel_id = 7;
    r.timestamp_utc_ms = ts_ms;
    return r;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_self_model_behavior_log_init_zeroed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Pre-fill with garbage to prove init wipes the struct cleanly. */
    hu_agent_behavior_log_t log;
    memset(&log, 0xCC, sizeof(log));

    hu_error_t rc = hu_agent_behavior_log_init(&log, &alloc, 16);
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_NOT_NULL(log.records);
    HU_ASSERT_EQ((long long)log.capacity, 16);
    HU_ASSERT_EQ((long long)log.head, 0);

    /* AC-SM-1 fixture probe: snapshot on a fresh log returns zero. */
    hu_agent_behavior_record_t out[4];
    size_t out_count = 99;
    rc = hu_agent_behavior_log_snapshot(&log, out, 4, &out_count);
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_EQ((long long)out_count, 0);

    hu_agent_behavior_log_destroy(&log);
    HU_ASSERT(log.records == NULL);
    HU_ASSERT_EQ((long long)log.capacity, 0);
}

static void test_self_model_behavior_log_init_default_capacity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));

    /* capacity=0 selects the documented default. */
    hu_error_t rc = hu_agent_behavior_log_init(&log, &alloc, 0);
    HU_ASSERT_EQ(rc, HU_OK);
    HU_ASSERT_EQ((long long)log.capacity, (long long)HU_AGENT_BEHAVIOR_LOG_DEFAULT_CAPACITY);
    hu_agent_behavior_log_destroy(&log);
}

static void test_self_model_behavior_log_init_rejects_oversized(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));

    hu_error_t rc =
        hu_agent_behavior_log_init(&log, &alloc, HU_AGENT_BEHAVIOR_LOG_MAX_CAPACITY + 1);
    HU_ASSERT_EQ(rc, HU_ERR_INVALID_ARGUMENT);
    /* Init must leave the log in a safe resting state on failure. */
    HU_ASSERT(log.records == NULL);
    HU_ASSERT_EQ((long long)log.capacity, 0);
}

static void test_self_model_behavior_log_init_rejects_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t rc = hu_agent_behavior_log_init(NULL, &alloc, 16);
    HU_ASSERT_EQ(rc, HU_ERR_INVALID_ARGUMENT);

    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    rc = hu_agent_behavior_log_init(&log, NULL, 16);
    HU_ASSERT_EQ(rc, HU_ERR_INVALID_ARGUMENT);
}

static void test_self_model_behavior_log_record_advances_head(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 8), HU_OK);

    /* AC-SM-1 pin: three fixture turns → head advances by exactly three. */
    hu_agent_behavior_record_t r0 = make_record(100, 0xA1, 1000);
    hu_agent_behavior_record_t r1 = make_record(200, 0xB2, 2000);
    hu_agent_behavior_record_t r2 = make_record(300, 0xC3, 3000);

    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r0), HU_OK);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r1), HU_OK);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r2), HU_OK);

    HU_ASSERT_EQ((long long)hu_agent_behavior_log_total_records(&log), 3);
    HU_ASSERT_EQ((long long)log.head, 3);

    /* Recorded fields round-trip through snapshot. */
    hu_agent_behavior_record_t out[8];
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 8, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 3);
    HU_ASSERT_EQ((long long)out[0].response_length_chars, 100);
    HU_ASSERT_EQ((long long)out[0].tool_sequence_hash, 0xA1);
    HU_ASSERT_EQ((long long)out[1].response_length_chars, 200);
    HU_ASSERT_EQ((long long)out[2].response_length_chars, 300);
    HU_ASSERT_EQ((long long)out[2].timestamp_utc_ms, 3000);

    hu_agent_behavior_log_destroy(&log);
}

static void test_self_model_behavior_log_record_wraps_at_capacity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    /* Tiny capacity to exercise wrap deterministically. */
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 4), HU_OK);

    /* Record 10 turns into a 4-slot ring; expect total=10, capacity=4,
     * and snapshot to return the most recent 4 in chronological order. */
    for (int i = 0; i < 10; i++) {
        hu_agent_behavior_record_t r =
            make_record((uint32_t)(i * 10), (uint32_t)i, (int64_t)(i * 100));
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }
    HU_ASSERT_EQ((long long)hu_agent_behavior_log_total_records(&log), 10);
    HU_ASSERT_EQ((long long)log.capacity, 4);
    HU_ASSERT_EQ((long long)log.head, 10);

    hu_agent_behavior_record_t out[8];
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 8, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 4);
    /* Most recent 4 chronologically: i = 6,7,8,9 → length = 60,70,80,90 */
    HU_ASSERT_EQ((long long)out[0].response_length_chars, 60);
    HU_ASSERT_EQ((long long)out[1].response_length_chars, 70);
    HU_ASSERT_EQ((long long)out[2].response_length_chars, 80);
    HU_ASSERT_EQ((long long)out[3].response_length_chars, 90);
    HU_ASSERT_EQ((long long)out[0].timestamp_utc_ms, 600);
    HU_ASSERT_EQ((long long)out[3].timestamp_utc_ms, 900);

    hu_agent_behavior_log_destroy(&log);
}

static void test_self_model_behavior_log_snapshot_returns_recent_n(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 16), HU_OK);

    /* Insert 5 records, snapshot the most recent 3. */
    for (int i = 0; i < 5; i++) {
        hu_agent_behavior_record_t r =
            make_record((uint32_t)(i + 1), (uint32_t)(0x1000 + i), (int64_t)(i + 1));
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }

    hu_agent_behavior_record_t out[3];
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 3, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 3);
    /* Most recent 3 are records 3, 4, 5 (1-indexed). */
    HU_ASSERT_EQ((long long)out[0].response_length_chars, 3);
    HU_ASSERT_EQ((long long)out[1].response_length_chars, 4);
    HU_ASSERT_EQ((long long)out[2].response_length_chars, 5);

    /* max_out=0 returns count=0 with HU_OK. */
    out_count = 99;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 0, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 0);

    /* NULL out_count → invalid args. */
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 3, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_agent_behavior_log_destroy(&log);
}

static void test_self_model_behavior_log_snapshot_wraps_boundary(void) {
    /* The wrap path inside snapshot splits the copy across the end of
     * the slab. Exercise that path explicitly: capacity 4, head=7, then
     * ask for the 4 most recent — they live at slots 2, 3, 0, 1. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 4), HU_OK);

    for (int i = 0; i < 7; i++) {
        hu_agent_behavior_record_t r = make_record((uint32_t)(i + 1), 0, (int64_t)(i + 1));
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }

    hu_agent_behavior_record_t out[4];
    size_t out_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 4, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 4);
    /* Records 4, 5, 6, 7 in chronological order. */
    HU_ASSERT_EQ((long long)out[0].response_length_chars, 4);
    HU_ASSERT_EQ((long long)out[1].response_length_chars, 5);
    HU_ASSERT_EQ((long long)out[2].response_length_chars, 6);
    HU_ASSERT_EQ((long long)out[3].response_length_chars, 7);

    hu_agent_behavior_log_destroy(&log);
}

static void test_self_model_behavior_log_zero_alloc_per_record(void) {
    /* Hot-path invariant: record() does ZERO heap allocations. Use the
     * project's tracking allocator to count alloc events around init →
     * record × N → destroy. Exactly one alloc (init slab) + one free
     * (destroy slab), regardless of how many records flow through. */
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    HU_ASSERT_NOT_NULL(ta);
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 32), HU_OK);
    size_t allocated_after_init = hu_tracking_allocator_total_allocated(ta);
    HU_ASSERT_GT((long long)allocated_after_init, 0);

    /* Record many turns; total_allocated must NOT advance. */
    for (int i = 0; i < 100; i++) {
        hu_agent_behavior_record_t r = make_record((uint32_t)i, (uint32_t)i, (int64_t)i);
        HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    }
    size_t allocated_after_records = hu_tracking_allocator_total_allocated(ta);
    HU_ASSERT_EQ((long long)allocated_after_records, (long long)allocated_after_init);

    hu_agent_behavior_log_destroy(&log);
    /* destroy frees the slab — no leaks. */
    HU_ASSERT_EQ((long long)hu_tracking_allocator_leaks(ta), 0);
    hu_tracking_allocator_destroy(ta);
}

static void test_self_model_behavior_log_record_null_log_is_noop(void) {
    /* Hot path tolerates a NULL log without crashing or allocating. */
    hu_agent_behavior_record_t r = make_record(42, 0xFEED, 12345);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(NULL, &r), HU_OK);
}

static void test_self_model_behavior_log_record_null_rec_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 8), HU_OK);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, NULL), HU_OK);
    HU_ASSERT_EQ((long long)log.head, 0);
    hu_agent_behavior_log_destroy(&log);
}

/* #2 self-model readback: the directive must REFLECT the window (mean length +
 * dominant tone), refuse <3 turns, and reject NULL args. Non-vacuous: asserts
 * the computed mean (250) and tone string actually appear. */
static void test_self_model_build_directive_reflects_window(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_behavior_log_t log;
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 16), HU_OK);

    char *dir = NULL;
    size_t dir_len = 0;

    /* <3 turns → no directive (NULL, HU_OK) */
    hu_agent_behavior_record_t a = make_record(100, 0x1, 1000);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &a), HU_OK);
    HU_ASSERT_EQ(hu_agent_self_model_build_directive(&log, &alloc, &dir, &dir_len), HU_OK);
    HU_ASSERT_TRUE(dir == NULL);
    HU_ASSERT_EQ((long long)dir_len, 0);

    /* 4 POSITIVE turns, lengths 100/200/300/400 → mean 250, dominant tone warm */
    hu_agent_behavior_record_t b = make_record(200, 0x2, 2000);
    hu_agent_behavior_record_t c = make_record(300, 0x3, 3000);
    hu_agent_behavior_record_t d = make_record(400, 0x4, 4000);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &b), HU_OK);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &c), HU_OK);
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &d), HU_OK);

    HU_ASSERT_EQ(hu_agent_self_model_build_directive(&log, &alloc, &dir, &dir_len), HU_OK);
    HU_ASSERT_TRUE(dir != NULL);
    HU_ASSERT_TRUE(dir_len > 0);
    HU_ASSERT_TRUE(strstr(dir, "self-awareness") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "250") != NULL);               /* mean of 100..400 */
    HU_ASSERT_TRUE(strstr(dir, "warm and positive") != NULL); /* dominant register */
    alloc.free(alloc.ctx, dir, dir_len + 1);

    /* NULL log → INVALID_ARGUMENT */
    HU_ASSERT_EQ(hu_agent_self_model_build_directive(NULL, &alloc, &dir, &dir_len),
                 HU_ERR_INVALID_ARGUMENT);

    hu_agent_behavior_log_destroy(&log);
}

#else /* !HU_ENABLE_SELF_MODEL */

/* In OFF builds we still want to verify the documented contract that
 * record() is a harmless no-op against an uninitialized log. That's
 * AC-SM-6's central claim — without it, callers would need conditional
 * compilation, defeating the purpose of the flag. */
static void test_self_model_disabled_flag_record_is_noop(void) {
    hu_agent_behavior_log_t log;
    memset(&log, 0, sizeof(log));

    /* init returns HU_OK and leaves the log zeroed. */
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_agent_behavior_log_init(&log, &alloc, 16), HU_OK);

    /* record is a no-op; head stays 0. */
    hu_agent_behavior_record_t r;
    memset(&r, 0, sizeof(r));
    r.response_length_chars = 99;
    HU_ASSERT_EQ(hu_agent_behavior_log_record(&log, &r), HU_OK);
    HU_ASSERT_EQ((long long)hu_agent_behavior_log_total_records(&log), 0);

    /* snapshot reports zero records. */
    hu_agent_behavior_record_t out[4];
    size_t out_count = 99;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&log, out, 4, &out_count), HU_OK);
    HU_ASSERT_EQ((long long)out_count, 0);

    /* destroy is safe. */
    hu_agent_behavior_log_destroy(&log);
}

#endif /* HU_ENABLE_SELF_MODEL */

void run_self_model_behavior_log_tests(void);
void run_self_model_behavior_log_tests(void) {
    HU_TEST_SUITE("self_model_behavior_log");
#ifdef HU_ENABLE_SELF_MODEL
    HU_RUN_TEST(test_self_model_behavior_log_init_zeroed);
    HU_RUN_TEST(test_self_model_behavior_log_init_default_capacity);
    HU_RUN_TEST(test_self_model_behavior_log_init_rejects_oversized);
    HU_RUN_TEST(test_self_model_behavior_log_init_rejects_null_args);
    HU_RUN_TEST(test_self_model_behavior_log_record_advances_head);
    HU_RUN_TEST(test_self_model_behavior_log_record_wraps_at_capacity);
    HU_RUN_TEST(test_self_model_behavior_log_snapshot_returns_recent_n);
    HU_RUN_TEST(test_self_model_behavior_log_snapshot_wraps_boundary);
    HU_RUN_TEST(test_self_model_behavior_log_zero_alloc_per_record);
    HU_RUN_TEST(test_self_model_behavior_log_record_null_log_is_noop);
    HU_RUN_TEST(test_self_model_behavior_log_record_null_rec_is_noop);
    HU_RUN_TEST(test_self_model_build_directive_reflects_window);
#else
    HU_RUN_TEST(test_self_model_disabled_flag_record_is_noop);
#endif
}
