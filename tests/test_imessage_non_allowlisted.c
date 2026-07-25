/*
 * US-9.3: iMessage non-allowlisted sender courtesy reply.
 *
 * Pins all 5 ACs (AC-9.3.1 through AC-9.3.5) via 18 tests:
 *   • Pure predicate truth table (7 tests).
 *   • Pure text-builder safety (3 tests).
 *   • Persistent dedup-log behavior (3 tests).
 *   • Integration via hu_imessage_test_handle_non_allowlisted (3 tests).
 *   • AC-9.3.3 chat.db BUSY one-shot warn (2 tests).
 *
 * Every assertion is a POSITIVE observable per
 * `.claude/rules/tests-that-pin-bugs.md`:
 *   • last_courtesy_message[0] != '\0' for AC-9.3.1
 *   • last_courtesy_message[0] == '\0' on second send within 24h (AC-9.3.2)
 *   • file content / dedup count / log emitted bool — never `rc == HU_OK`.
 *
 * References `hu_imessage_*` production symbols throughout (see
 * imports below) — satisfies AC-9.3.5 and
 * `.claude/rules/test-references-production-symbol.md`. The test covers
 * `src/channels/imessage.c` symbols (hu_imessage_should_courtesy_reply,
 * hu_imessage_courtesy_dedup_record, hu_imessage_build_courtesy_reply,
 * hu_imessage_test_handle_non_allowlisted, etc.), but the auto-heuristic
 * in `scripts/check-test-references.sh` resolves the bare module name
 * "imessage" to `src/feeds/imessage.c` (alphabetical first match of two
 * files named imessage.c). The opt-out below is intentional — this is
 * the "wrong module picked by heuristic" case, not a standalone test.
 *
 * // @covers-none — heuristic picks wrong src/imessage.c; see header above.
 */
#if HU_HAS_IMESSAGE
#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if HU_IS_TEST

/* ── Test fixture: per-test HOME under /tmp so the dedup log is isolated ── */

static char g_test_home[256];
static char g_saved_home[256];

static void hu_imessage_us93_setup(void) {
    const char *prior = getenv("HOME");
    if (prior)
        snprintf(g_saved_home, sizeof(g_saved_home), "%s", prior);
    else
        g_saved_home[0] = '\0';
    snprintf(g_test_home, sizeof(g_test_home), "/tmp/hu_us93_%d_%ld", (int)getpid(),
             (long)time(NULL));
    (void)mkdir(g_test_home, 0700);
    setenv("HOME", g_test_home, 1);
    /* Pre-create the .human/ subdirectory so file ops succeed deterministically. */
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/.human", g_test_home);
    (void)mkdir(dir, 0700);
    /* Remove any stale dedup log from a previous test in the same process. */
    char path[512];
    if (hu_imessage_courtesy_log_path(path, sizeof(path)))
        (void)unlink(path);
}

static void hu_imessage_us93_teardown(void) {
    char path[512];
    if (hu_imessage_courtesy_log_path(path, sizeof(path)))
        (void)unlink(path);
    char dir[400];
    snprintf(dir, sizeof(dir), "%s/.human", g_test_home);
    (void)rmdir(dir);
    (void)rmdir(g_test_home);
    if (g_saved_home[0])
        setenv("HOME", g_saved_home, 1);
    else
        unsetenv("HOME");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 1 — Pure predicate truth table (AC-9.3.1, AC-9.3.2)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void us93_predicate_non_allowlisted_first_time_returns_true(void) {
    /* Handle is NOT in allowlist, no prior reply this bucket, feature on,
     * aggregate cap not blown → predicate must say "send". */
    HU_ASSERT_TRUE(hu_imessage_should_courtesy_reply(
        false /* not allowlisted */, false /* not yet replied */, true /* enabled */, 0));
}

static void us93_predicate_non_allowlisted_second_time_in_bucket_returns_false(void) {
    /* Same as above but we already replied this bucket — must NOT send again. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, true /* already replied */, true, 1));
}

static void us93_predicate_allowlisted_handle_returns_false(void) {
    /* If the caller is asking about an allowlisted handle, predicate is
     * false regardless of any other input. (The poll loop only asks for
     * non-allowlisted handles, but we defend in depth.) */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(true /* allowlisted */, false, true, 0));
}

static void us93_predicate_disabled_by_config_returns_false(void) {
    /* Operator opt-out: courtesy_replies_enabled=false → predicate is false. */
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, false, false /* disabled */, 0));
}

static void us93_predicate_aggregate_cap_blocks_further_replies(void) {
    /* Spoof-spam mitigation: when the per-bucket aggregate has hit the
     * cap, even a "first-time" handle is dropped to keep total daily
     * outbound iMessage volume bounded. */
    HU_ASSERT_FALSE(
        hu_imessage_should_courtesy_reply(false, false, true, HU_IMESSAGE_COURTESY_DAILY_CAP));
    HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(false, false, true,
                                                      HU_IMESSAGE_COURTESY_DAILY_CAP + 100));
}

static void us93_predicate_is_pure_no_side_effects(void) {
    /* Call 100x with identical inputs — same result each time, no I/O. */
    for (int i = 0; i < 100; i++) {
        HU_ASSERT_TRUE(hu_imessage_should_courtesy_reply(false, false, true, 0));
        HU_ASSERT_FALSE(hu_imessage_should_courtesy_reply(true, false, true, 0));
    }
}

static void us93_predicate_aggregate_below_cap_still_allows(void) {
    /* One under the cap is fine; the cap is inclusive on the "no" side. */
    HU_ASSERT_TRUE(
        hu_imessage_should_courtesy_reply(false, false, true, HU_IMESSAGE_COURTESY_DAILY_CAP - 1));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 2 — Pure text-builder safety
 * ═══════════════════════════════════════════════════════════════════════════ */

static void us93_reply_text_mentions_allowlist_word(void) {
    char buf[1024];
    size_t n = hu_imessage_build_courtesy_reply("Atlas", "Jane", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(buf, "allowlist");
}

static void us93_reply_text_omits_owner_handle_when_only_display_name_given(void) {
    char buf[1024];
    size_t n = hu_imessage_build_courtesy_reply("Atlas", "Jane", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(buf, "Jane");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "+1");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "@");
}

static void us93_reply_text_safe_with_null_persona_name(void) {
    char buf[1024];
    size_t n = hu_imessage_build_courtesy_reply(NULL, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(buf, "allowlist");
    /* Generic fallbacks present; no raw handles. */
    HU_ASSERT_STR_NOT_CONTAINS(buf, "+1");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "@");
}

static void us93_reply_text_strips_handle_shaped_persona_name(void) {
    /* Defense in depth: if a caller mistakenly passes a phone-shaped string
     * as a persona name, the builder MUST fall back to the generic phrasing
     * rather than echoing it back. */
    char buf[1024];
    size_t n =
        hu_imessage_build_courtesy_reply("+15551234567", "jane@example.com", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "+15551234567");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "jane@example.com");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 3 — Persistent dedup-log behavior
 * ═══════════════════════════════════════════════════════════════════════════ */

static void us93_dedup_record_then_check_returns_true_same_bucket(void) {
    hu_imessage_us93_setup();
    HU_ASSERT_FALSE(hu_imessage_courtesy_dedup_check("+15550001111", 19854));
    hu_imessage_courtesy_dedup_record("+15550001111", 19854);
    HU_ASSERT_TRUE(hu_imessage_courtesy_dedup_check("+15550001111", 19854));
    hu_imessage_us93_teardown();
}

static void us93_dedup_check_returns_false_for_next_bucket(void) {
    hu_imessage_us93_setup();
    hu_imessage_courtesy_dedup_record("+15550001111", 19854);
    HU_ASSERT_TRUE(hu_imessage_courtesy_dedup_check("+15550001111", 19854));
    HU_ASSERT_FALSE(hu_imessage_courtesy_dedup_check("+15550001111", 19855));
    hu_imessage_us93_teardown();
}

static void us93_dedup_aggregate_count_grows_per_bucket(void) {
    hu_imessage_us93_setup();
    HU_ASSERT_EQ(hu_imessage_courtesy_aggregate_count(19854), (uint32_t)0);
    hu_imessage_courtesy_dedup_record("+15550001111", 19854);
    hu_imessage_courtesy_dedup_record("+15550002222", 19854);
    hu_imessage_courtesy_dedup_record("+15550003333", 19854);
    HU_ASSERT_EQ(hu_imessage_courtesy_aggregate_count(19854), (uint32_t)3);
    /* Different bucket → 0. */
    HU_ASSERT_EQ(hu_imessage_courtesy_aggregate_count(19855), (uint32_t)0);
    hu_imessage_us93_teardown();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 4 — Integration via hu_imessage_test_handle_non_allowlisted
 *           (positive observables per tests-that-pin-bugs.md)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void us93_non_allowlisted_first_dm_triggers_courtesy_reply(void) {
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    /* Drive the pipeline at mock epoch -> bucket = 1e6 / 86400 = 11. */
    HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, "+15558675309", 1000000), HU_OK);

    /* AC-9.3.4: positive observable — the courtesy mirror is populated. */
    size_t mirror_len = 0;
    const char *mirror = hu_imessage_test_get_last_courtesy_message(&ch, &mirror_len);
    HU_ASSERT_NOT_NULL(mirror);
    HU_ASSERT_TRUE(mirror[0] != '\0');
    HU_ASSERT_TRUE(mirror_len > 0);
    /* AC-9.3.1 content contract: reply explains the allowlist. */
    HU_ASSERT_STR_CONTAINS(mirror, "allowlist");
    /* The dedup log now reflects the send. */
    HU_ASSERT_TRUE(hu_imessage_courtesy_dedup_check("+15558675309", 1000000 / 86400));

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

static void us93_non_allowlisted_second_dm_within_24h_no_reply(void) {
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    /* First DM at epoch=1,000,000 → bucket 11. Mirror gets set. */
    HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, "+15558675309", 1000000), HU_OK);
    HU_ASSERT_TRUE(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0] != '\0');

    /* Clear the mirror. Inject a second DM 1 hour later — STILL bucket 11. */
    hu_imessage_test_clear_last_courtesy_message(&ch);
    HU_ASSERT_EQ(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0], '\0');

    HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, "+15558675309", 1000000 + 3600),
                 HU_OK);

    /* AC-9.3.2 positive observable: mirror is STILL empty (no second send). */
    HU_ASSERT_EQ(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0], '\0');

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

static void us93_non_allowlisted_dm_in_next_bucket_triggers_second_reply(void) {
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    /* First DM, bucket B. */
    HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, "+15558675309", 1000000), HU_OK);
    HU_ASSERT_TRUE(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0] != '\0');

    /* Advance >= 86400 sec → bucket B+1. Clear mirror. Second DM should
     * trigger a brand-new courtesy reply. */
    hu_imessage_test_clear_last_courtesy_message(&ch);
    HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, "+15558675309", 1000000 + 86400),
                 HU_OK);
    HU_ASSERT_TRUE(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0] != '\0');
    HU_ASSERT_STR_CONTAINS(hu_imessage_test_get_last_courtesy_message(&ch, NULL), "allowlist");

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

static void us93_aggregate_cap_blocks_after_50_handles(void) {
    /* Spoof-spam adversary: 50 unique handles in one bucket → 50 replies,
     * 51st spoof gets nothing. */
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    int64_t epoch = 1000000;
    for (int i = 0; i < HU_IMESSAGE_COURTESY_DAILY_CAP; i++) {
        char handle[32];
        snprintf(handle, sizeof(handle), "+1555%07d", i);
        HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, handle, epoch), HU_OK);
    }
    HU_ASSERT_EQ(hu_imessage_courtesy_aggregate_count(epoch / 86400),
                 (uint32_t)HU_IMESSAGE_COURTESY_DAILY_CAP);

    /* Clear mirror; spoof the (CAP+1)th handle. */
    hu_imessage_test_clear_last_courtesy_message(&ch);
    HU_ASSERT_EQ(hu_imessage_test_handle_non_allowlisted(&ch, "+15559999999", epoch), HU_OK);

    /* Positive observable: NO reply emitted on the 51st send. */
    HU_ASSERT_EQ(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0], '\0');

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 5 — AC-9.3.3: chat.db BUSY-exhaustion one-shot warn
 * ═══════════════════════════════════════════════════════════════════════════ */

static void us93_chatdb_busy_exhaustion_sets_one_shot_gate(void) {
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    HU_ASSERT_FALSE(hu_imessage_test_chatdb_busy_log_emitted(&ch));
    hu_imessage_test_record_chatdb_busy_exhaustion(&ch);

    /* Positive observable: the one-shot gate is now armed → the warn was
     * emitted exactly once for this episode. */
    HU_ASSERT_TRUE(hu_imessage_test_chatdb_busy_log_emitted(&ch));

    /* AC-9.3.3 also requires save_poll_status to have been called: the
     * status file should exist at the expected path. */
    char path[512];
    HU_ASSERT_TRUE(hu_imessage_status_path(path, sizeof(path)));
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

static void us93_chatdb_busy_log_is_one_shot_per_episode(void) {
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    /* 10 consecutive BUSY exhaustions → gate stays armed, warn emitted once. */
    for (int i = 0; i < 10; i++)
        hu_imessage_test_record_chatdb_busy_exhaustion(&ch);
    HU_ASSERT_TRUE(hu_imessage_test_chatdb_busy_log_emitted(&ch));

    /* Simulate a successful poll resetting the gate. Next BUSY episode
     * re-arms exactly once more. */
    hu_imessage_test_reset_chatdb_busy_gate(&ch);
    HU_ASSERT_FALSE(hu_imessage_test_chatdb_busy_log_emitted(&ch));
    hu_imessage_test_record_chatdb_busy_exhaustion(&ch);
    HU_ASSERT_TRUE(hu_imessage_test_chatdb_busy_log_emitted(&ch));

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 6 — exclude_from: handles the assistant must never message
 *
 * Distinct from the allowlist above. A non-allowlisted sender GETS a courtesy
 * reply; an EXCLUDED one must get absolute silence, because the exclusion
 * exists for a real person who asked not to be texted by the assistant — an
 * automated "you're not allowlisted" bounce would be the very harm the switch
 * is meant to prevent.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void exclude_matches_exact_handle(void) {
    const char *list[] = {"+15558675309"};
    HU_ASSERT_TRUE(hu_imessage_handle_excluded("+15558675309", list, 1));
}

static void exclude_matches_across_handle_formatting(void) {
    /* The failure that matters: an allowlist that misses safely denies, but an
     * exclusion that misses SENDS to the one person who must not be messaged.
     * Every real-world spelling of the same number must match. */
    const char *list[] = {"+15558675309"};
    HU_ASSERT_TRUE(hu_imessage_handle_excluded("15558675309", list, 1));
    HU_ASSERT_TRUE(hu_imessage_handle_excluded("5558675309", list, 1));
    HU_ASSERT_TRUE(hu_imessage_handle_excluded("+1 (555) 867-5309", list, 1));
    HU_ASSERT_TRUE(hu_imessage_handle_excluded("555-867-5309", list, 1));
}

static void exclude_does_not_match_a_different_number(void) {
    /* Over-matching must stop at genuinely different people. */
    const char *list[] = {"+15558675309"};
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("+15551112222", list, 1));
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("+15558675300", list, 1));
}

static void exclude_matches_email_handle_case_insensitively(void) {
    const char *list[] = {"Someone@Example.com"};
    HU_ASSERT_TRUE(hu_imessage_handle_excluded("someone@example.com", list, 1));
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("other@example.com", list, 1));
}

static void exclude_empty_list_excludes_nobody(void) {
    /* Default posture: with no exclusions configured nothing is blocked. */
    const char *list[] = {"+15558675309"};
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("+15558675309", NULL, 0));
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("+15558675309", list, 0));
    HU_ASSERT_FALSE(hu_imessage_handle_excluded(NULL, list, 1));
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("", list, 1));
}

static void exclude_short_digit_string_does_not_mass_exclude(void) {
    /* A stray short entry must not silently blackhole every contact whose
     * number happens to end in those digits. Both sides need a full tail. */
    const char *list[] = {"5309"};
    HU_ASSERT_FALSE(hu_imessage_handle_excluded("+15558675309", list, 1));
}

static void exclude_blocks_outbound_send(void) {
    /* The contract that makes "never texts her" true for PROACTIVE messages,
     * not just replies: imessage_send is the single outbound chokepoint. */
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    static const char *const excl[] = {"+15558675309"};
    hu_imessage_set_exclude_from(&ch, excl, 1);

    /* Excluded target is refused outright... */
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15558675309", 12, "hey", 3, NULL, 0),
                 HU_ERR_PERMISSION_DENIED);
    /* ...including a differently-formatted spelling of the same number... */
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+1 (555) 867-5309", 17, "hey", 3, NULL, 0),
                 HU_ERR_PERMISSION_DENIED);
    /* ...while everyone else still sends normally. */
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15551112222", 12, "hey", 3, NULL, 0), HU_OK);

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

static void exclude_inbound_sends_no_courtesy_reply(void) {
    /* The whole point: an excluded sender gets SILENCE, never the
     * "you're not on the allowlist" bounce a non-allowlisted sender gets. */
    hu_imessage_us93_setup();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "Jane", 4, NULL, 0, &ch), HU_OK);

    static const char *const excl[] = {"+15558675309"};
    hu_imessage_set_exclude_from(&ch, excl, 1);

    /* Drive the courtesy path directly for an EXCLUDED handle. Even here --
     * the deepest point the poll loop could reach -- the outbound guard must
     * keep the mirror empty. */
    (void)hu_imessage_test_handle_non_allowlisted(&ch, "+15558675309", 1000000);
    HU_ASSERT_EQ(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0], '\0');

    /* Control: a non-excluded handle on the same channel still gets one, so
     * the assertion above is proving exclusion and not a dead code path. */
    (void)hu_imessage_test_handle_non_allowlisted(&ch, "+15551112222", 1000000);
    HU_ASSERT_TRUE(hu_imessage_test_get_last_courtesy_message(&ch, NULL)[0] != '\0');

    hu_imessage_destroy(&ch);
    hu_imessage_us93_teardown();
}

void run_imessage_non_allowlisted_tests(void) {
    HU_TEST_SUITE("iMessage Non-Allowlisted Courtesy Reply (US-9.3)");

    /* Part 1: pure predicate truth table */
    HU_RUN_TEST(us93_predicate_non_allowlisted_first_time_returns_true);
    HU_RUN_TEST(us93_predicate_non_allowlisted_second_time_in_bucket_returns_false);
    HU_RUN_TEST(us93_predicate_allowlisted_handle_returns_false);
    HU_RUN_TEST(us93_predicate_disabled_by_config_returns_false);
    HU_RUN_TEST(us93_predicate_aggregate_cap_blocks_further_replies);
    HU_RUN_TEST(us93_predicate_is_pure_no_side_effects);
    HU_RUN_TEST(us93_predicate_aggregate_below_cap_still_allows);

    /* Part 2: text builder */
    HU_RUN_TEST(us93_reply_text_mentions_allowlist_word);
    HU_RUN_TEST(us93_reply_text_omits_owner_handle_when_only_display_name_given);
    HU_RUN_TEST(us93_reply_text_safe_with_null_persona_name);
    HU_RUN_TEST(us93_reply_text_strips_handle_shaped_persona_name);

    /* Part 3: dedup file */
    HU_RUN_TEST(us93_dedup_record_then_check_returns_true_same_bucket);
    HU_RUN_TEST(us93_dedup_check_returns_false_for_next_bucket);
    HU_RUN_TEST(us93_dedup_aggregate_count_grows_per_bucket);

    /* Part 4: integration */
    HU_RUN_TEST(us93_non_allowlisted_first_dm_triggers_courtesy_reply);
    HU_RUN_TEST(us93_non_allowlisted_second_dm_within_24h_no_reply);
    HU_RUN_TEST(us93_non_allowlisted_dm_in_next_bucket_triggers_second_reply);
    HU_RUN_TEST(us93_aggregate_cap_blocks_after_50_handles);

    /* Part 5: AC-9.3.3 chat.db BUSY warn */
    HU_RUN_TEST(us93_chatdb_busy_exhaustion_sets_one_shot_gate);
    HU_RUN_TEST(us93_chatdb_busy_log_is_one_shot_per_episode);

    /* Part 6: exclude_from — never message these handles */
    HU_RUN_TEST(exclude_matches_exact_handle);
    HU_RUN_TEST(exclude_matches_across_handle_formatting);
    HU_RUN_TEST(exclude_does_not_match_a_different_number);
    HU_RUN_TEST(exclude_matches_email_handle_case_insensitively);
    HU_RUN_TEST(exclude_empty_list_excludes_nobody);
    HU_RUN_TEST(exclude_short_digit_string_does_not_mass_exclude);
    HU_RUN_TEST(exclude_blocks_outbound_send);
    HU_RUN_TEST(exclude_inbound_sends_no_courtesy_reply);
}

#else  /* !HU_IS_TEST */
void run_imessage_non_allowlisted_tests(void) {
    (void)0;
}
#endif /* HU_IS_TEST */

#else  /* !HU_HAS_IMESSAGE */
void run_imessage_non_allowlisted_tests(void) {
    (void)0;
}
#endif /* HU_HAS_IMESSAGE */
