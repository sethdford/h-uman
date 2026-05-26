#include "human/channels/imessage_reply.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

/* Test counters + stubs. */
static int tier1_call_count = 0;
static int tier2_call_count = 0;

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

static void reset_counts(void) {
    tier1_call_count = 0;
    tier2_call_count = 0;
}

/* AC: Tier 1 (Cmd-R) is attempted first; on success, returns HU_OK
 * and records "cmdR" as the tier used. */
static void tier1_cmd_r_succeeds_returns_ok(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(tier1_call_count, 1);
    HU_ASSERT_STR_EQ(hu_imessage_test_last_reply_tier(), "cmdR");

    hu_imessage_set_test_reply_stubs(NULL, NULL);
}

/* AC: Tier 1 failure (without C2/C3 yet) returns NOT_SUPPORTED. */
static void tier1_fails_returns_not_supported_until_c2_lands(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_fails_stub, NULL);

    hu_error_t err = hu_imessage_reply(NULL, "+15555551212", 12, "ABC-PARENT-GUID", 15, "Hey", 3);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(tier1_call_count, 1);

    hu_imessage_set_test_reply_stubs(NULL, NULL);
}

/* AC: Invalid args return INVALID_ARGUMENT without touching any tier. */
static void invalid_args_short_circuit(void) {
    reset_counts();
    hu_imessage_set_test_reply_stubs(tier1_succeeds_stub, NULL);

    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, NULL, 0, "G", 1, "b", 1),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_imessage_reply(NULL, "+15555551212", 12, "G", 1, NULL, 0),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(tier1_call_count, 0);

    hu_imessage_set_test_reply_stubs(NULL, NULL);
}

void run_imessage_threaded_reply_tests(void) {
    HU_TEST_SUITE("imessage_threaded_reply");
    HU_RUN_TEST(tier1_cmd_r_succeeds_returns_ok);
    HU_RUN_TEST(tier1_fails_returns_not_supported_until_c2_lands);
    HU_RUN_TEST(invalid_args_short_circuit);
}
