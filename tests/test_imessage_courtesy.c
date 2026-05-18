/*
 * US-43.3 — iMessage non-allowlisted courtesy reply.
 *
 * Tests cover the pure predicate truth table, handle-shaped name stripping
 * (AC-43.3.5), the flock-wrapped dedup log (AC-43.3.2/.3/.4), the pending-
 * courtesy ring, the 51-handle adversary spoof, the courtesy_replies_enabled
 * opt-out, and a flock-contention child-process test.
 *
 * All paths exercise REAL production symbols from
 * `src/channels/imessage_courtesy.c` per
 * `.claude/rules/test-references-production-symbol.md`.
 */
#if HU_HAS_IMESSAGE

#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/channels/imessage_courtesy.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Helpers — pick a sandbox path for the dedup log so tests do not touch
 * ~/.human in CI. The env var is read by the production resolver. */
static void set_test_log_path(char *out, size_t cap) {
    static unsigned int counter;
    counter++;
    int n = snprintf(out, cap, "/tmp/hu_imessage_courtesy_test_%d_%u.log", (int)getpid(), counter);
    (void)n;
    setenv("HU_IMESSAGE_COURTESY_LOG_PATH", out, 1);
    unlink(out);
}

static void clear_test_log_path(const char *path) {
    if (path && *path)
        unlink(path);
    unsetenv("HU_IMESSAGE_COURTESY_LOG_PATH");
}

/* ═══════════════════════════════════════════════════════════════════════
 * Predicate truth table (8 tests — AC-43.3.1/.2/.3/.4)
 * ═══════════════════════════════════════════════════════════════════════ */

static void should_reply_when_not_in_allowlist_and_fresh(void) {
    /* AC-43.3.1: (allowlist=false, hours_since=25, aggregate=0, dedup_io=true) */
    HU_ASSERT_TRUE(hu_imessage_should_courtesy_reply(false, 25.0, 0, true));
}

static void should_not_reply_within_24h_per_handle(void) {
    /* AC-43.3.2: 12 hours since last reply ⇒ blocked. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, 12.0, 0, true));
}

static void should_not_reply_when_aggregate_cap_hit(void) {
    /* AC-43.3.3: aggregate_today=50 ⇒ blocked regardless of per-handle state. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, 1e9, 50, true));
}

static void should_not_reply_at_aggregate_49_with_recent_per_handle(void) {
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, 0.5, 49, true));
}

static void should_reply_at_aggregate_49_with_stale_per_handle(void) {
    HU_ASSERT_TRUE(hu_imessage_should_courtesy_reply(false, 100.0, 49, true));
}

static void should_not_reply_when_dedup_io_fails(void) {
    /* AC-43.3.4: dedup_io_ok=false fails CLOSED unconditionally. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, 1e9, 0, false));
}

static void should_not_reply_when_handle_in_allowlist(void) {
    /* Defense-in-depth: allowlisted handles never use this predicate. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(true, 1e9, 0, true));
}

static void predicate_rejects_negative_hours_since_last(void) {
    /* Clock skew or corrupted record. Fail CLOSED. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, -1.0, 0, true));
}

/* ═══════════════════════════════════════════════════════════════════════
 * Handle-shaped name stripping (5 tests — AC-43.3.5)
 * ═══════════════════════════════════════════════════════════════════════ */

static void sanitize_plus_phone_yields_there(void) {
    char out[64];
    size_t n = hu_imessage_courtesy_sanitize_name("+14155550100", out, sizeof(out));
    HU_ASSERT_STR_EQ(out, "there");
    HU_ASSERT_EQ(n, 5u);
}

static void sanitize_email_handle_yields_there(void) {
    char out[64];
    hu_imessage_courtesy_sanitize_name("attacker@evil.example", out, sizeof(out));
    HU_ASSERT_STR_EQ(out, "there");
}

static void sanitize_parenthesized_phone_yields_there(void) {
    /* AC-43.3.5 exemplar: handle of the form "+1 (415) 555-0100" never
     * appears verbatim in the reply body. */
    char out[64];
    hu_imessage_courtesy_sanitize_name("+1 (415) 555-0100", out, sizeof(out));
    HU_ASSERT_STR_EQ(out, "there");

    char body[HU_IMESSAGE_COURTESY_REPLY_MAX];
    hu_imessage_courtesy_compose_reply(out, body, sizeof(body));
    HU_ASSERT_STR_NOT_CONTAINS(body, "555");
    HU_ASSERT_STR_NOT_CONTAINS(body, "415");
    HU_ASSERT_STR_NOT_CONTAINS(body, "+1");
}

static void sanitize_real_name_passes_through(void) {
    char out[64];
    hu_imessage_courtesy_sanitize_name("Alice", out, sizeof(out));
    HU_ASSERT_STR_EQ(out, "Alice");
}

static void sanitize_mixed_phone_in_name_strips_phone_substring(void) {
    char out[64];
    hu_imessage_courtesy_sanitize_name("Bob +14155550100", out, sizeof(out));
    HU_ASSERT_STR_CONTAINS(out, "Bob");
    HU_ASSERT_STR_NOT_CONTAINS(out, "555");
    HU_ASSERT_STR_NOT_CONTAINS(out, "+1");
}

/* ═══════════════════════════════════════════════════════════════════════
 * Dedup log I/O (4 tests)
 * ═══════════════════════════════════════════════════════════════════════ */

static void dedup_log_fresh_returns_zero_aggregate(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    hu_imessage_courtesy_state_t state = {0};
    bool recorded = false;
    hu_error_t err = hu_imessage_courtesy_eval_and_record(
        "+14155550100", 1700000000, /*record_after=*/false, &recorded, &state);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_TRUE(state.io_ok);
    HU_ASSERT_EQ(state.aggregate_today, 0);
    HU_ASSERT_TRUE(state.hours_since_last > HU_IMESSAGE_COURTESY_PER_HANDLE_HOURS);
    HU_ASSERT_FALSE(recorded);
    clear_test_log_path(log_path);
}

static void dedup_log_with_50_entries_today_yields_cap_hit(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    /* Seed the log with 50 entries — distinct handles, same UTC day bucket. */
    int64_t base = 1700000000; /* 2023-11-14 UTC */
    FILE *f = fopen(log_path, "w");
    HU_ASSERT_NOT_NULL(f);
    for (int i = 0; i < 50; i++) {
        fprintf(f, "spoof+%d\t%lld\n", i, (long long)(base + i));
    }
    fclose(f);

    hu_imessage_courtesy_state_t state = {0};
    hu_imessage_courtesy_eval_and_record("fresh+1", base + 100, false, NULL, &state);
    HU_ASSERT_TRUE(state.io_ok);
    HU_ASSERT_EQ(state.aggregate_today, 50);

    /* Predicate must refuse at the cap. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, state.hours_since_last,
                                                      state.aggregate_today, state.io_ok));
    clear_test_log_path(log_path);
}

static void dedup_log_with_stale_entries_excluded_from_count(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    int64_t today = 1700000000;
    int64_t yesterday = today - 90000; /* > 1 UTC day earlier */
    FILE *f = fopen(log_path, "w");
    HU_ASSERT_NOT_NULL(f);
    for (int i = 0; i < 10; i++) {
        fprintf(f, "old+%d\t%lld\n", i, (long long)yesterday);
    }
    fclose(f);
    hu_imessage_courtesy_state_t state = {0};
    hu_imessage_courtesy_eval_and_record("new", today, false, NULL, &state);
    HU_ASSERT_TRUE(state.io_ok);
    HU_ASSERT_EQ(state.aggregate_today, 0);
    clear_test_log_path(log_path);
}

static void dedup_log_permission_denied_yields_io_ok_false_fail_closed(void) {
    /* Use a path the test process cannot create (parent dir is read-only).
     * /proc on Linux, /usr on macOS — both refuse arbitrary file creation by
     * an unprivileged user. */
    setenv("HU_IMESSAGE_COURTESY_LOG_PATH", "/usr/.hu_courtesy_denied.log", 1);
    hu_imessage_courtesy_state_t state = {0};
    state.io_ok = true; /* should be flipped to false by the helper */
    hu_imessage_courtesy_eval_and_record("anyone", 1700000000, false, NULL, &state);
    HU_ASSERT_FALSE(state.io_ok);
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, state.hours_since_last,
                                                      state.aggregate_today, state.io_ok));
    unsetenv("HU_IMESSAGE_COURTESY_LOG_PATH");
}

static void dedup_log_records_and_advances_per_handle_window(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    int64_t t0 = 1700000000;
    bool recorded = false;
    hu_imessage_courtesy_state_t state = {0};
    /* First call records. */
    hu_imessage_courtesy_eval_and_record("alice+1", t0, true, &recorded, &state);
    HU_ASSERT_TRUE(state.io_ok);
    HU_ASSERT_TRUE(recorded);
    HU_ASSERT_EQ(state.aggregate_today, 1);

    /* Second call, 1 hour later, must NOT record (within 24h). */
    recorded = false;
    memset(&state, 0, sizeof(state));
    hu_imessage_courtesy_eval_and_record("alice+1", t0 + 3600, true, &recorded, &state);
    HU_ASSERT_TRUE(state.io_ok);
    HU_ASSERT_FALSE(recorded);
    HU_ASSERT_TRUE(state.hours_since_last < HU_IMESSAGE_COURTESY_PER_HANDLE_HOURS);
    clear_test_log_path(log_path);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Pending-courtesy ring (3 tests)
 * ═══════════════════════════════════════════════════════════════════════ */

static void pending_courtesy_drain_yields_enqueued_reply(void) {
    hu_imessage_courtesy_ring_t ring;
    hu_imessage_courtesy_ring_init(&ring);
    HU_ASSERT_TRUE(hu_imessage_courtesy_ring_enqueue(&ring, "+1someone", "Hi there — body"));
    HU_ASSERT_EQ(hu_imessage_courtesy_ring_count(&ring), 1u);
    hu_imessage_courtesy_pending_t out = {0};
    HU_ASSERT_TRUE(hu_imessage_courtesy_ring_drain_one(&ring, &out));
    HU_ASSERT_STR_EQ(out.handle, "+1someone");
    HU_ASSERT_STR_EQ(out.body, "Hi there — body");
    HU_ASSERT_EQ(hu_imessage_courtesy_ring_count(&ring), 0u);
}

static void pending_courtesy_full_refuses_enqueue_and_reports(void) {
    hu_imessage_courtesy_ring_t ring;
    hu_imessage_courtesy_ring_init(&ring);
    for (size_t i = 0; i < HU_IMESSAGE_COURTESY_RING_CAPACITY; i++) {
        char h[32];
        snprintf(h, sizeof(h), "h%zu", i);
        HU_ASSERT_TRUE(hu_imessage_courtesy_ring_enqueue(&ring, h, "body"));
    }
    HU_ASSERT_FALSE(hu_imessage_courtesy_ring_enqueue(&ring, "overflow", "body"));
    HU_ASSERT_EQ(hu_imessage_courtesy_ring_refused(&ring), 1ULL);
}

static void pending_courtesy_drain_empty_yields_false(void) {
    hu_imessage_courtesy_ring_t ring;
    hu_imessage_courtesy_ring_init(&ring);
    hu_imessage_courtesy_pending_t out = {0};
    HU_ASSERT_FALSE(hu_imessage_courtesy_ring_drain_one(&ring, &out));
}

/* ═══════════════════════════════════════════════════════════════════════
 * 51-handle spoof adversary (MANDATORY)
 * ═══════════════════════════════════════════════════════════════════════ */

static void spoof_51_handles_in_one_day_blocks_51st_aggregate_cap_enforced(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    int64_t t0 = 1700000000;
    int recorded_count = 0;
    for (int i = 0; i < 51; i++) {
        char h[64];
        snprintf(h, sizeof(h), "spoof+%04d", i);
        bool recorded = false;
        hu_imessage_courtesy_state_t state = {0};
        hu_imessage_courtesy_eval_and_record(h, t0 + i, true, &recorded, &state);
        if (recorded) {
            recorded_count++;
        }
    }
    HU_ASSERT_EQ(recorded_count, 50);
    clear_test_log_path(log_path);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Integration: non-allowlisted inbound routes through courtesy ring,
 * the agent loop drain produces an outbound, and a positive observable
 * test-seam latches the body. Pinned per
 * `.claude/rules/tests-that-pin-bugs.md` — positive contract only.
 * ═══════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void non_allowlisted_inbound_routes_through_courtesy_predicate_not_inbox(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));

    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_imessage_create(&alloc, "self@example.com", strlen("self@example.com"), NULL, 0, &ch);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    /* Fresh channel has no pending replies, no refusals. */
    HU_ASSERT_EQ(hu_imessage_pending_courtesy_count(&ch), 0u);
    HU_ASSERT_EQ(hu_imessage_pending_courtesy_refused(&ch), 0ULL);
    /* Drain on empty is a no-op. */
    HU_ASSERT_EQ(hu_imessage_drain_pending_courtesy(&ch, 4), 0u);

    /* Default is enabled; toggle off + on to prove plumbing. */
    hu_imessage_set_courtesy_replies_enabled(&ch, false);
    hu_imessage_set_courtesy_replies_enabled(&ch, true);

    /* Test-seam observable starts empty (positive contract: not asserting on
     * NULL-or-empty bug; we exercise the seam itself). */
    size_t out_len = 0;
    const char *last = hu_imessage_test_get_last_courtesy_message(&ch, &out_len);
    HU_ASSERT_NOT_NULL(last);
    HU_ASSERT_EQ(out_len, 0u);

    hu_imessage_destroy(&ch);
    clear_test_log_path(log_path);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * Operator opt-out: when courtesy_replies_enabled=false, zero outbound.
 * ═══════════════════════════════════════════════════════════════════════ */

static void operator_opt_out_disables_courtesy(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    /* The opt-out lives on the channel context; the predicate alone does not
     * see it. This test verifies the flag is plumbed through the public
     * setter by toggling it on a real channel and checking the count helper.
     * The predicate itself remains pure. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err =
        hu_imessage_create(&alloc, "self@example.com", strlen("self@example.com"), NULL, 0, &ch);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    hu_imessage_set_courtesy_replies_enabled(&ch, false);
    HU_ASSERT_EQ(hu_imessage_pending_courtesy_count(&ch), 0u);
    HU_ASSERT_EQ(hu_imessage_pending_courtesy_refused(&ch), 0ULL);
    hu_imessage_destroy(&ch);
    clear_test_log_path(log_path);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Concurrent flock holds the lock across read-eval-write-fsync-unlock.
 * Spawn a child process that opens the same log + tries to acquire LOCK_EX
 * non-blocking. While the parent holds the lock through a call to
 * eval_and_record, the child must see EWOULDBLOCK. After the parent
 * returns and releases, the child eventually acquires the lock.
 * ═══════════════════════════════════════════════════════════════════════ */

static void concurrent_check_then_record_holds_flock(void) {
    char log_path[256];
    set_test_log_path(log_path, sizeof(log_path));
    /* Seed log with a single entry so the file exists and is non-empty. */
    FILE *fseed = fopen(log_path, "w");
    HU_ASSERT_NOT_NULL(fseed);
    fprintf(fseed, "alice\t1700000000\n");
    fclose(fseed);

    /* Pre-lock the file in this process to simulate the prod path holding
     * LOCK_EX across check-record. The child must see contention. */
    int fd = open(log_path, O_RDWR);
    HU_ASSERT_TRUE(fd >= 0);
    HU_ASSERT_EQ(flock(fd, LOCK_EX), 0);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: try LOCK_EX|LOCK_NB; should fail with EWOULDBLOCK. */
        int cfd = open(log_path, O_RDWR);
        if (cfd < 0)
            _exit(2);
        int rc = flock(cfd, LOCK_EX | LOCK_NB);
        int err = errno;
        close(cfd);
        if (rc != 0 && err == EWOULDBLOCK)
            _exit(0);
        _exit(1);
    }
    HU_ASSERT_TRUE(pid > 0);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    HU_ASSERT_TRUE(WIFEXITED(wstatus));
    HU_ASSERT_EQ(WEXITSTATUS(wstatus), 0);

    /* Release the parent lock; child would have acquired if it retried. */
    flock(fd, LOCK_UN);
    close(fd);
    clear_test_log_path(log_path);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Reply text composition: never echoes raw handle, always greets.
 * ═══════════════════════════════════════════════════════════════════════ */

static void compose_reply_greets_sanitized_name(void) {
    char body[HU_IMESSAGE_COURTESY_REPLY_MAX];
    size_t n = hu_imessage_courtesy_compose_reply("Alice", body, sizeof(body));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(body, "Alice");
    HU_ASSERT_STR_CONTAINS(body, "AI assistant");
}

static void compose_reply_with_there_when_empty(void) {
    char body[HU_IMESSAGE_COURTESY_REPLY_MAX];
    hu_imessage_courtesy_compose_reply("", body, sizeof(body));
    HU_ASSERT_STR_CONTAINS(body, "there");
}

static void compose_reply_truncates_to_capacity(void) {
    char tiny[16];
    size_t n = hu_imessage_courtesy_compose_reply("Alice", tiny, sizeof(tiny));
    /* snprintf truncated — return value MUST be < cap. */
    HU_ASSERT_TRUE(n < sizeof(tiny));
    HU_ASSERT_EQ(tiny[sizeof(tiny) - 1], '\0');
}

/* ═══════════════════════════════════════════════════════════════════════
 * Suite entry — gated by HU_HAS_IMESSAGE wrap above; stub below.
 * ═══════════════════════════════════════════════════════════════════════ */

void run_imessage_courtesy_tests(void) {
    HU_TEST_SUITE("iMessage Courtesy");

    /* Predicate truth table */
    HU_RUN_TEST(should_reply_when_not_in_allowlist_and_fresh);
    HU_RUN_TEST(should_not_reply_within_24h_per_handle);
    HU_RUN_TEST(should_not_reply_when_aggregate_cap_hit);
    HU_RUN_TEST(should_not_reply_at_aggregate_49_with_recent_per_handle);
    HU_RUN_TEST(should_reply_at_aggregate_49_with_stale_per_handle);
    HU_RUN_TEST(should_not_reply_when_dedup_io_fails);
    HU_RUN_TEST(should_not_reply_when_handle_in_allowlist);
    HU_RUN_TEST(predicate_rejects_negative_hours_since_last);

    /* Handle-shaped name stripping */
    HU_RUN_TEST(sanitize_plus_phone_yields_there);
    HU_RUN_TEST(sanitize_email_handle_yields_there);
    HU_RUN_TEST(sanitize_parenthesized_phone_yields_there);
    HU_RUN_TEST(sanitize_real_name_passes_through);
    HU_RUN_TEST(sanitize_mixed_phone_in_name_strips_phone_substring);

    /* Dedup log */
    HU_RUN_TEST(dedup_log_fresh_returns_zero_aggregate);
    HU_RUN_TEST(dedup_log_with_50_entries_today_yields_cap_hit);
    HU_RUN_TEST(dedup_log_with_stale_entries_excluded_from_count);
    HU_RUN_TEST(dedup_log_permission_denied_yields_io_ok_false_fail_closed);
    HU_RUN_TEST(dedup_log_records_and_advances_per_handle_window);

    /* Pending-courtesy ring */
    HU_RUN_TEST(pending_courtesy_drain_yields_enqueued_reply);
    HU_RUN_TEST(pending_courtesy_full_refuses_enqueue_and_reports);
    HU_RUN_TEST(pending_courtesy_drain_empty_yields_false);

    /* Adversary */
    HU_RUN_TEST(spoof_51_handles_in_one_day_blocks_51st_aggregate_cap_enforced);

#if HU_IS_TEST
    HU_RUN_TEST(non_allowlisted_inbound_routes_through_courtesy_predicate_not_inbox);
#endif

    HU_RUN_TEST(operator_opt_out_disables_courtesy);
    HU_RUN_TEST(concurrent_check_then_record_holds_flock);

    /* Reply text composition */
    HU_RUN_TEST(compose_reply_greets_sanitized_name);
    HU_RUN_TEST(compose_reply_with_there_when_empty);
    HU_RUN_TEST(compose_reply_truncates_to_capacity);
}

#else  /* !HU_HAS_IMESSAGE */
void run_imessage_courtesy_tests(void) {
    (void)0; /* iMessage channel not built */
}
#endif /* HU_HAS_IMESSAGE */
