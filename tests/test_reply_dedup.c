/* Truth-table + persistence guard for outbound reply idempotency (BUG #2).
 * Pins hu_daemon_already_replied (the pure skip/proceed decision) and the
 * save/load round-trip that makes the dedup survive a daemon crash+restart. */
#include "human/daemon/reply_dedup.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *chat_a = "+12393005206";
static const char *chat_b = "+14845661687";

/* Brand-new contact: nothing recorded -> must proceed (not already replied). */
static void already_replied_unknown_contact_returns_false(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1000));
}

/* Same inbound rowid replayed after a reply (the crash case) -> skip. */
static void already_replied_same_rowid_returns_true(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    HU_ASSERT_TRUE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1000));
}

/* An older rowid than the watermark -> also already replied -> skip. */
static void already_replied_older_rowid_returns_true(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    HU_ASSERT_TRUE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 999));
}

/* A newer inbound rowid -> not yet replied -> proceed. */
static void already_replied_newer_rowid_returns_false(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1001));
}

/* Per-contact isolation: a reply to chat_a must not suppress chat_b. */
static void already_replied_per_contact_isolation(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, chat_b, strlen(chat_b), 50));
    HU_ASSERT_TRUE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1000));
}

/* Watermark is monotonic: an out-of-order replay of an older rowid must not
 * lower it (else a still-older replay could slip through). */
static void record_is_monotonic(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 500); /* out-of-order older */
    HU_ASSERT_TRUE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1000));
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1001));
}

/* NULL / non-positive rowid are safe and conservative (proceed, don't skip). */
static void already_replied_null_and_zero_safe(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    HU_ASSERT_FALSE(hu_daemon_already_replied(NULL, chat_a, strlen(chat_a), 1000));
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, NULL, 0, 1000));
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 0));
    /* rowid<=0 record is a no-op. */
    hu_reply_dedup_t r2;
    memset(&r2, 0, sizeof(r2));
    hu_reply_dedup_record(&r2, chat_a, strlen(chat_a), 0);
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r2, chat_a, strlen(chat_a), 1));
}

/* Crash-survival: record + save, then a FRESH store loads and still reports the
 * inbound as already-replied (the whole point of persisting after send). */
static void save_load_round_trip_survives_restart(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_reply_dedup_test_%d.json", (int)getpid());
    remove(path);

    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1000);
    hu_reply_dedup_record(&r, chat_b, strlen(chat_b), 42);
    HU_ASSERT_EQ((int)hu_reply_dedup_save(&r, path, strlen(path)), (int)HU_OK);

    hu_reply_dedup_t loaded;
    memset(&loaded, 0xFF, sizeof(loaded)); /* poison to prove load resets */
    HU_ASSERT_EQ((int)hu_reply_dedup_load(&loaded, path, strlen(path)), (int)HU_OK);

    HU_ASSERT_TRUE(hu_daemon_already_replied(&loaded, chat_a, strlen(chat_a), 1000));
    HU_ASSERT_TRUE(hu_daemon_already_replied(&loaded, chat_b, strlen(chat_b), 42));
    HU_ASSERT_FALSE(hu_daemon_already_replied(&loaded, chat_a, strlen(chat_a), 1001));
    /* chat_b watermark is 42; a newer inbound proceeds. */
    HU_ASSERT_FALSE(hu_daemon_already_replied(&loaded, chat_b, strlen(chat_b), 43));

    remove(path);
}

/* Crash-resume with a NEW TAIL appended to the batch must NOT be deduped.
 *
 * Pins the contract that the daemon keys dedup on the batch's HIGHEST rowid
 * (batch_end), not the lowest (batch_start). Scenario: a batch [1000..1002] is
 * replied to (watermark recorded at the highest = 1002). The daemon crashes
 * before the poll watermark saves; on restart the user has appended msg 1003,
 * so the re-polled batch is [1000..1002, 1003]. Keying on the highest rowid
 * (1003) correctly reports NOT-already-replied so the new tail gets a reply;
 * keying on the lowest (1000) would report already-replied and silently drop
 * the new message. */
static void crash_resume_with_new_tail_not_deduped(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    /* Replied to batch [1000..1002] -> record the HIGHEST rowid in the batch. */
    hu_reply_dedup_record(&r, chat_a, strlen(chat_a), 1002);
    /* Exact replay of the same batch (highest rowid 1002) -> skip. */
    HU_ASSERT_TRUE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1002));
    /* Replay-plus-new-tail: the batch's highest rowid is now 1003 (the new
     * message). Must NOT be deduped — otherwise 1003 is silently dropped. */
    HU_ASSERT_FALSE(hu_daemon_already_replied(&r, chat_a, strlen(chat_a), 1003));
}

/* Absent file -> HU_ERR_IO and the store is left usable as empty. */
static void load_missing_file_returns_io_error(void) {
    hu_reply_dedup_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_EQ((int)hu_reply_dedup_load(&r, "/tmp/hu_reply_dedup_does_not_exist_zzz.json", 42),
                 (int)HU_ERR_IO);
}

void run_reply_dedup_tests(void) {
    HU_TEST_SUITE("reply_dedup");
    HU_RUN_TEST(already_replied_unknown_contact_returns_false);
    HU_RUN_TEST(already_replied_same_rowid_returns_true);
    HU_RUN_TEST(already_replied_older_rowid_returns_true);
    HU_RUN_TEST(already_replied_newer_rowid_returns_false);
    HU_RUN_TEST(already_replied_per_contact_isolation);
    HU_RUN_TEST(record_is_monotonic);
    HU_RUN_TEST(already_replied_null_and_zero_safe);
    HU_RUN_TEST(crash_resume_with_new_tail_not_deduped);
    HU_RUN_TEST(save_load_round_trip_survives_restart);
    HU_RUN_TEST(load_missing_file_returns_io_error);
}
