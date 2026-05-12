/*
 * tests/test_channel_trust.c — SOTA-2026 init-09 §2.10 smoke gate.
 *
 * Pins the channel → trust-tier mapping that downstream gates (write-trust
 * recall verifier, MINJA quarantine, persona-delta sourcing) read from
 * every incoming message. The classifier is the choke-point for the entire
 * trust ladder; a silent regression here would let third-party content
 * promote itself to USER_DIRECT and bypass MINJA scanning, so the contract
 * is worth pinning even with a small smoke suite.
 */

#include "human/agent/channel_trust.h"
#include "human/memory/trust.h"
#include "test_framework.h"

#include <string.h>

static void channel_trust_self_channels_are_user_direct(void) {
    /* CLI / stdin are the canonical 1:1 typed-by-user surfaces. */
    HU_ASSERT_EQ((int)hu_channel_trust("cli", 3), (int)HU_TRUST_USER_DIRECT);
    HU_ASSERT_EQ((int)hu_channel_trust("stdin", 5), (int)HU_TRUST_USER_DIRECT);
}

static void channel_trust_self_prefix_is_first_party(void) {
    /* "self:..." tool-origin channels (calendar, email, etc.) are
     * FIRST_PARTY — user-installed but not directly typed. */
    HU_ASSERT_EQ((int)hu_channel_trust("self:email", 10), (int)HU_TRUST_FIRST_PARTY);
    HU_ASSERT_EQ((int)hu_channel_trust("self_email", 10), (int)HU_TRUST_FIRST_PARTY);
}

static void channel_trust_unqualified_telegram_is_third_party(void) {
    /* The classifier intentionally refuses to promote unqualified "telegram"
     * to one-to-one — that ambiguity is the exact thing init-09 is closing.
     * Channel handlers must qualify ("telegram_dm" or "telegram_group"). */
    HU_ASSERT_EQ((int)hu_channel_trust("telegram", 8), (int)HU_TRUST_THIRD_PARTY);
}

static void channel_trust_qualified_dm_is_user_direct(void) {
    /* DM = 1:1 with a paired contact — the channel handler vouches
     * for the binding, so the classifier treats it as USER_DIRECT.
     * Group chats with the same prefix stay at THIRD_PARTY. */
    HU_ASSERT_EQ((int)hu_channel_trust("telegram_dm", 11), (int)HU_TRUST_USER_DIRECT);
    HU_ASSERT_EQ((int)hu_channel_trust("discord_dm", 10), (int)HU_TRUST_USER_DIRECT);
    HU_ASSERT_EQ((int)hu_channel_trust("imessage", 8), (int)HU_TRUST_USER_DIRECT);
}

static void channel_trust_group_chat_is_third_party(void) {
    HU_ASSERT_EQ((int)hu_channel_trust("telegram_group", 14), (int)HU_TRUST_THIRD_PARTY);
    HU_ASSERT_EQ((int)hu_channel_trust("discord_group", 13), (int)HU_TRUST_THIRD_PARTY);
}

static void channel_trust_unknown_channel_is_third_party(void) {
    /* Conservative default: anything we don't recognize gets the
     * lowest tier so the recall verifier re-validates it. */
    HU_ASSERT_EQ((int)hu_channel_trust("zzz_unknown_channel", 19), (int)HU_TRUST_THIRD_PARTY);
    HU_ASSERT_EQ((int)hu_channel_trust("", 0), (int)HU_TRUST_THIRD_PARTY);
    HU_ASSERT_EQ((int)hu_channel_trust(NULL, 0), (int)HU_TRUST_THIRD_PARTY);
}

static void channel_trust_one_to_one_helper_matches_classifier(void) {
    /* hu_channel_is_one_to_one must agree with the classifier — the
     * write-trust gate uses one to deny the other. */
    HU_ASSERT(hu_channel_is_one_to_one("cli", 3));
    HU_ASSERT_FALSE(hu_channel_is_one_to_one("telegram_group", 14));
    HU_ASSERT_FALSE(hu_channel_is_one_to_one("", 0));
    HU_ASSERT_FALSE(hu_channel_is_one_to_one(NULL, 0));
}

static void channel_trust_stamp_round_trips_channel(void) {
    hu_provenance_t p = hu_channel_trust_stamp("cli", 3, NULL, 0, 1700000000LL);
    HU_ASSERT_EQ((int)p.tier, (int)HU_TRUST_USER_DIRECT);
    HU_ASSERT_EQ((long long)p.source_ts, 1700000000LL);
    HU_ASSERT_STR_EQ(p.channel, "cli");
}

static void channel_trust_stamp_with_handle_keeps_unknown_third_party(void) {
    /* A handle on an unknown channel should NOT promote tier — the
     * classifier is the source of truth, not the presence of a handle. */
    hu_provenance_t p = hu_channel_trust_stamp("zzz_unknown", 11, "alice", 5, 42LL);
    HU_ASSERT_EQ((int)p.tier, (int)HU_TRUST_THIRD_PARTY);
    HU_ASSERT_EQ((long long)p.source_ts, 42LL);
}

void run_channel_trust_tests(void) {
    HU_TEST_SUITE("channel_trust");
    HU_RUN_TEST(channel_trust_self_channels_are_user_direct);
    HU_RUN_TEST(channel_trust_self_prefix_is_first_party);
    HU_RUN_TEST(channel_trust_unqualified_telegram_is_third_party);
    HU_RUN_TEST(channel_trust_qualified_dm_is_user_direct);
    HU_RUN_TEST(channel_trust_group_chat_is_third_party);
    HU_RUN_TEST(channel_trust_unknown_channel_is_third_party);
    HU_RUN_TEST(channel_trust_one_to_one_helper_matches_classifier);
    HU_RUN_TEST(channel_trust_stamp_round_trips_channel);
    HU_RUN_TEST(channel_trust_stamp_with_handle_keeps_unknown_third_party);
}
