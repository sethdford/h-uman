#include "human/channels/imessage_reply.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test counters + stubs. */
static int tier1_call_count = 0;
static int tier2_call_count = 0;
static int flat_send_call_count = 0;

static bool tier1_succeeds_stub(const char *guid, size_t guid_len, const char *body,
                                size_t body_len) {
    (void)guid;
    (void)guid_len;
    (void)body;
    (void)body_len;
    tier1_call_count++;
    return true;
}

static bool tier1_fails_stub(const char *guid, size_t guid_len, const char *body, size_t body_len) {
    (void)guid;
    (void)guid_len;
    (void)body;
    (void)body_len;
    tier1_call_count++;
    return false;
}

/* Tier 2 stubs. */
static bool tier2_succeeds_stub(const char *guid, size_t guid_len, const char *body,
                                size_t body_len) {
    (void)guid;
    (void)guid_len;
    (void)body;
    (void)body_len;
    tier2_call_count++;
    return true;
}

static bool tier2_fails_stub(const char *guid, size_t guid_len, const char *body, size_t body_len) {
    (void)guid;
    (void)guid_len;
    (void)body;
    (void)body_len;
    tier2_call_count++;
    return false;
}

/* Tier 3 flat-send stubs. */
static hu_error_t flat_send_succeeds_stub(const char *target, size_t target_len, const char *body,
                                          size_t body_len) {
    (void)target;
    (void)target_len;
    (void)body;
    (void)body_len;
    flat_send_call_count++;
    return HU_OK;
}

static hu_error_t flat_send_fails_stub(const char *target, size_t target_len, const char *body,
                                       size_t body_len) {
    (void)target;
    (void)target_len;
    (void)body;
    (void)body_len;
    flat_send_call_count++;
    return HU_ERR_INTERNAL;
}

/* Post-send chat.db threading verify stubs. */
static int verify_call_count = 0;

static bool verify_true_stub(const char *target, size_t target_len, int64_t since_rowid) {
    (void)target;
    (void)target_len;
    (void)since_rowid;
    verify_call_count++;
    return true;
}

static bool verify_false_stub(const char *target, size_t target_len, int64_t since_rowid) {
    (void)target;
    (void)target_len;
    (void)since_rowid;
    verify_call_count++;
    return false;
}

static void reset_counts(void) {
    tier1_call_count = 0;
    tier2_call_count = 0;
    flat_send_call_count = 0;
    verify_call_count = 0;
}

/* AC: Tier 1 (Cmd-R) is attempted first; on success, returns HU_OK
 * and records "cmdR" as the tier used. */
static void tier1_cmd_r_succeeds_returns_ok(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "cmdR");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Tier 1 failure with no Tier 2 stub returns NOT_SUPPORTED. */
static void tier1_fails_with_no_tier2_returns_not_supported(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, NULL, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(tier1_call_count, 1);

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Invalid args return INVALID_ARGUMENT without touching any tier. */
static void invalid_args_short_circuit(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL, NULL);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, NULL, 0, "G", 1, "b", 1),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "G", 1, NULL, 0),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(tier1_call_count, 0);

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: When Tier 1 fails, Tier 2 (AXShowMenu) is attempted; on success
 * the dispatcher records "ax_menu" tier. */
static void tier2_ax_menu_falls_through_from_tier1(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_succeeds_stub, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_EQ(tier2_call_count, 1);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "ax_menu");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Tier 1 success skips Tier 2 entirely (no needless work). */
static void tier1_success_skips_tier2(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, tier2_succeeds_stub, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_EQ(tier2_call_count, 0); /* never called */
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "cmdR");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Both AX tiers fail, Tier 3 falls back with NO stub = NOT_SUPPORTED. */
static void both_tiers_fail_no_flat_stub_returns_not_supported(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_fails_stub, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_EQ(tier2_call_count, 1);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "flat_fallback");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: When BOTH AX tiers fail, Tier 3 (flat send) is attempted. */
static void tier3_flat_fallback_when_both_ax_tiers_fail(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_fails_stub, flat_send_succeeds_stub);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_EQ(tier2_call_count, 1);
    HU_ASSERT_EQ(flat_send_call_count, 1);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "flat_fallback");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Tier 3 propagates the flat-send result on failure. */
static void tier3_propagates_flat_send_error(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_fails_stub, flat_send_fails_stub);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INTERNAL);
    HU_ASSERT_EQ(flat_send_call_count, 1);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "flat_fallback");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Tier 3 not reached when an upstream tier succeeds. */
static void tier3_not_called_when_tier1_succeeds(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, tier2_succeeds_stub,
                                     flat_send_succeeds_stub);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_EQ(tier2_call_count, 0);
    HU_ASSERT_EQ(flat_send_call_count, 0);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "cmdR");

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Telemetry is emitted on Tier 1 success — one JSONL line per call. */
static void telemetry_emitted_on_tier1_success(void) {
    /* Use HU_IMESSAGE_ACTION_LOG_DIR env to isolate to tmp. */
    char tmpdir[] = "/tmp/human-c4-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL, NULL);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "GUID", 4, "hi", 2), (int)HU_OK);

    /* Verify exactly one JSONL line was written. */
    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    int line_count = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        line_count++;
        if (line_count == 1) {
            /* First line: verify "tier":"cmdR" is present. */
            HU_ASSERT(strstr(buf, "\"tier\":\"cmdR\"") != NULL);
        }
    }
    fclose(f);
    HU_ASSERT_EQ(line_count, 1);

    /* Cleanup. */
    unlink(path);
    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Telemetry tier reflects which fallback fired. */
static void telemetry_tier_reflects_fallback_used(void) {
    char tmpdir[] = "/tmp/human-c4-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier1_fails_stub, flat_send_succeeds_stub);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "GUID", 4, "hi", 2), (int)HU_OK);

    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[1024];
    HU_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
    fclose(f);
    HU_ASSERT(strstr(buf, "\"tier\":\"flat_fallback\"") != NULL);

    unlink(path);
    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC: Invalid args short-circuit BEFORE telemetry (no log line on error). */
static void no_telemetry_on_invalid_args(void) {
    char tmpdir[] = "/tmp/human-c4-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, NULL, 0, "G", 1, "b", 1),
                 (int)HU_ERR_INVALID_ARGUMENT);

    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    /* File should not exist (no line written). */
    FILE *f = fopen(path, "r");
    HU_ASSERT(f == NULL);

    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
}

/* AC (T15): the Tier-3 "AX unavailable" degradation WARN fires exactly
 * once per process even when the fallback is taken many times — so a busy
 * reply path doesn't spam the log. */
static void flat_fallback_warn_emitted_once_across_repeated_failures(void) {
    reset_counts();
    hu_imessage_test_reset_reply_warn();
    HU_ASSERT_EQ(hu_imessage_test_reply_warn_count(), 0);

    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_fails_stub, flat_send_succeeds_stub);
    for (int i = 0; i < 3; i++) {
        hu_error_t err =
            hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
        HU_ASSERT_EQ((int)err, (int)HU_OK);
        HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "flat_fallback");
    }
    /* Fallback taken 3 times, WARN emitted exactly once. */
    HU_ASSERT_EQ(flat_send_call_count, 3);
    HU_ASSERT_EQ(hu_imessage_test_reply_warn_count(), 1);

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
}

/* AC (T15): the reset hook clears the one-shot guard so the WARN can fire
 * again — keeps test isolation intact across suites. */
static void flat_fallback_warn_reset_hook_clears_counter(void) {
    reset_counts();
    hu_imessage_test_reset_reply_warn();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_fails_stub, flat_send_succeeds_stub);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "G", 1, "hi", 2), (int)HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_reply_warn_count(), 1);

    hu_imessage_test_reset_reply_warn();
    HU_ASSERT_EQ(hu_imessage_test_reply_warn_count(), 0);

    /* After reset, the next fallback re-emits exactly one WARN. */
    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "G", 1, "hi", 2), (int)HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_reply_warn_count(), 1);

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_test_reset_reply_warn();
}

/* AC: When the post-send chat.db read-back confirms a native thread, the
 * reply is reported as verified-threaded (getter true) AND telemetry records
 * style THREADED. The tier_used is still the AX tier that fired ("cmdR"). */
static void verified_threaded_reports_threaded_style(void) {
    char tmpdir[] = "/tmp/human-c4-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL, NULL);
    hu_imessage_set_test_reply_verify_stub(verify_true_stub);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "GUID", 4, "hi", 2), (int)HU_OK);
    HU_ASSERT_EQ(verify_call_count, 1);
    HU_ASSERT_TRUE(hu_imessage_reply_last_verified_threaded());
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "cmdR");

    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[1024];
    HU_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
    fclose(f);
    HU_ASSERT(strstr(buf, "\"style\":\"THREADED\"") != NULL);
    HU_ASSERT(strstr(buf, "\"tier\":\"cmdR\"") != NULL);

    unlink(path);
    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_set_test_reply_verify_stub(NULL);
}

/* AC: When the AX tier returns "true" but the chat.db read-back shows the
 * message committed FLAT (thread_originator_guid NULL), the reply is reported
 * truthfully — getter false AND telemetry style FLAT — WITHOUT a double-send.
 * The tier_used still reflects the AX tier that fired ("cmdR"). */
static void unverified_threaded_reports_flat_style_no_double_send(void) {
    char tmpdir[] = "/tmp/human-c4-XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(tmpdir));
    setenv("HU_IMESSAGE_ACTION_LOG_DIR", tmpdir, 1);

    reset_counts();
    /* flat_send stub present so we'd notice a double-send: it must NOT fire. */
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL, flat_send_succeeds_stub);
    hu_imessage_set_test_reply_verify_stub(verify_false_stub);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "GUID", 4, "hi", 2), (int)HU_OK);
    HU_ASSERT_EQ(verify_call_count, 1);
    HU_ASSERT_EQ(flat_send_call_count, 0); /* no double-send */
    HU_ASSERT(!hu_imessage_reply_last_verified_threaded());
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "cmdR");

    char path[512];
    snprintf(path, sizeof(path), "%s/imessage_action.jsonl", tmpdir);
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[1024];
    HU_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
    fclose(f);
    HU_ASSERT(strstr(buf, "\"style\":\"FLAT\"") != NULL);

    unlink(path);
    rmdir(tmpdir);
    unsetenv("HU_IMESSAGE_ACTION_LOG_DIR");
    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_set_test_reply_verify_stub(NULL);
}

/* AC: the verified-threaded flag is reset at the start of every call, so a
 * stale "true" from a prior verified reply never leaks into a later call that
 * falls through to flat-fallback (no verify attempted). */
static void verified_flag_resets_per_call(void) {
    reset_counts();
    /* First call: verified threaded → flag true. */
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL, NULL);
    hu_imessage_set_test_reply_verify_stub(verify_true_stub);
    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "G", 1, "hi", 2), (int)HU_OK);
    HU_ASSERT_TRUE(hu_imessage_reply_last_verified_threaded());

    /* Second call: both AX tiers fail → flat fallback, verify never runs,
     * flag must be back to false. */
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, tier2_fails_stub, flat_send_succeeds_stub);
    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "G", 1, "hi", 2), (int)HU_OK);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "flat_fallback");
    HU_ASSERT(!hu_imessage_reply_last_verified_threaded());

    hu_imessage_set_test_reply_stubs(NULL, NULL, NULL);
    hu_imessage_set_test_reply_verify_stub(NULL);
}

void run_imessage_threaded_reply_tests(void) {
    HU_TEST_SUITE("imessage_threaded_reply");
    HU_RUN_TEST(tier1_cmd_r_succeeds_returns_ok);
    HU_RUN_TEST(tier1_fails_with_no_tier2_returns_not_supported);
    HU_RUN_TEST(invalid_args_short_circuit);
    HU_RUN_TEST(tier2_ax_menu_falls_through_from_tier1);
    HU_RUN_TEST(tier1_success_skips_tier2);
    HU_RUN_TEST(both_tiers_fail_no_flat_stub_returns_not_supported);
    HU_RUN_TEST(tier3_flat_fallback_when_both_ax_tiers_fail);
    HU_RUN_TEST(tier3_propagates_flat_send_error);
    HU_RUN_TEST(tier3_not_called_when_tier1_succeeds);
    HU_RUN_TEST(telemetry_emitted_on_tier1_success);
    HU_RUN_TEST(telemetry_tier_reflects_fallback_used);
    HU_RUN_TEST(no_telemetry_on_invalid_args);
    HU_RUN_TEST(flat_fallback_warn_emitted_once_across_repeated_failures);
    HU_RUN_TEST(flat_fallback_warn_reset_hook_clears_counter);
    HU_RUN_TEST(verified_threaded_reports_threaded_style);
    HU_RUN_TEST(unverified_threaded_reports_flat_style_no_double_send);
    HU_RUN_TEST(verified_flag_resets_per_call);
}
