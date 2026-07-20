/* Tests for the iMessage capability probe (T0.4) — the gate every native
 * verb routes through. Pins the `imsg status` parse (including the
 * "Not available" ⊃ "available" substring trap per
 * ~/.claude/rules/substring-classifier-pitfalls.md) and the verb gating
 * contract: nothing advanced is ever attempted without a proven bridge. */

#include "human/channels/imessage_caps.h"
#include "test_framework.h"
#include <string.h>

/* Verbatim `imsg status` output from a Tahoe 26.5.1 box, SIP ENABLED. */
static const char *STATUS_SIP_ON = "imsg Status Report\n"
                                   "==================\n"
                                   "\n"
                                   "Version:\n"
                                   "  0.11.0\n"
                                   "\n"
                                   "Basic features (send, receive, history):\n"
                                   "  Available\n"
                                   "\n"
                                   "System Integrity Protection (SIP):\n"
                                   "  enabled\n"
                                   "\n"
                                   "Advanced features (typing, read receipts):\n"
                                   "  Not available\n"
                                   "\n"
                                   "To enable advanced features:\n"
                                   "  1. Disable System Integrity Protection (SIP)\n";

/* The shape we expect once the user disables SIP and runs `imsg launch`. */
static const char *STATUS_BRIDGE_LIVE = "imsg Status Report\n"
                                        "==================\n"
                                        "\n"
                                        "Basic features (send, receive, history):\n"
                                        "  Available\n"
                                        "\n"
                                        "System Integrity Protection (SIP):\n"
                                        "  disabled\n"
                                        "\n"
                                        "Advanced features (typing, read receipts):\n"
                                        "  Available\n";

static void caps_parse_sip_on_gates_advanced(void) {
    hu_imessage_caps_t caps;
    hu_error_t err = hu_imessage_caps_parse(STATUS_SIP_ON, strlen(STATUS_SIP_ON), &caps);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(caps.probed);
    HU_ASSERT_TRUE(caps.basic);
    /* THE trap: "Not available" contains "available". Must be FALSE. */
    HU_ASSERT_FALSE(caps.advanced);
    HU_ASSERT_TRUE(caps.sip_enabled);
}

static void caps_parse_bridge_live_enables_advanced(void) {
    hu_imessage_caps_t caps;
    hu_error_t err = hu_imessage_caps_parse(STATUS_BRIDGE_LIVE, strlen(STATUS_BRIDGE_LIVE), &caps);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(caps.basic);
    HU_ASSERT_TRUE(caps.advanced);
    HU_ASSERT_FALSE(caps.sip_enabled);
}

static void caps_parse_empty_or_null_is_no_capability(void) {
    hu_imessage_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_caps_parse(NULL, 0, &caps), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_imessage_caps_parse("", 0, &caps), HU_OK);
    /* Fail CLOSED: an unparseable status must never claim capability. */
    HU_ASSERT_FALSE(caps.basic);
    HU_ASSERT_FALSE(caps.advanced);
    HU_ASSERT_FALSE(caps.probed);
}

static void caps_parse_garbage_fails_closed(void) {
    const char *junk = "command not found: imsg\n";
    hu_imessage_caps_t caps;
    HU_ASSERT_EQ(hu_imessage_caps_parse(junk, strlen(junk), &caps), HU_OK);
    HU_ASSERT_FALSE(caps.basic);
    HU_ASSERT_FALSE(caps.advanced);
    HU_ASSERT_FALSE(caps.probed);
}

/* ── Verb gating: the contract that keeps us off UI puppetry ───────────── */

static void caps_gate_basic_verbs_need_only_basic(void) {
    hu_imessage_caps_t caps;
    (void)hu_imessage_caps_parse(STATUS_SIP_ON, strlen(STATUS_SIP_ON), &caps);
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_SEND));
}

static void caps_gate_advanced_verbs_blocked_without_bridge(void) {
    hu_imessage_caps_t caps;
    (void)hu_imessage_caps_parse(STATUS_SIP_ON, strlen(STATUS_SIP_ON), &caps);
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_REACT));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_REPLY_THREADED));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_TYPING));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_READ_RECEIPT));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_EDIT));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_UNSEND));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_EFFECT));
}

static void caps_gate_advanced_verbs_allowed_with_bridge(void) {
    hu_imessage_caps_t caps;
    (void)hu_imessage_caps_parse(STATUS_BRIDGE_LIVE, strlen(STATUS_BRIDGE_LIVE), &caps);
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_SEND));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_REACT));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_REPLY_THREADED));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_TYPING));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_READ_RECEIPT));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_EDIT));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_UNSEND));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_EFFECT));
}

static void caps_gate_null_caps_denies_everything(void) {
    /* An un-probed / NULL capability set must never authorize a native verb. */
    HU_ASSERT_FALSE(hu_imessage_caps_allows(NULL, HU_IMSG_VERB_SEND));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(NULL, HU_IMSG_VERB_REACT));
    hu_imessage_caps_t zero;
    memset(&zero, 0, sizeof(zero));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&zero, HU_IMSG_VERB_SEND));
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&zero, HU_IMSG_VERB_REACT));
}

static void caps_describe_is_operator_readable(void) {
    hu_imessage_caps_t caps;
    (void)hu_imessage_caps_parse(STATUS_SIP_ON, strlen(STATUS_SIP_ON), &caps);
    char buf[256];
    hu_imessage_caps_describe(&caps, buf, sizeof(buf));
    HU_ASSERT_STR_CONTAINS(buf, "basic=yes");
    HU_ASSERT_STR_CONTAINS(buf, "bridge=no");
    HU_ASSERT_STR_CONTAINS(buf, "sip=on");
}

/* Verbatim `imsg status` from this Tahoe 26.5.1 box with SIP off, library
 * validation disabled, and the bridge injected. NOTE the selector reality:
 * editMessage is ✗ on macOS 26 even though the bridge is fully live — the
 * documented edit selectors do not exist here. Unsend (retractMessagePart)
 * IS available. A verb gate that keys only off "bridge live" would silently
 * emit failing edit calls. */
static const char *STATUS_BRIDGE_V2_TAHOE = "Basic features (send, receive, history):\n"
                                            "  Available\n"
                                            "\n"
                                            "System Integrity Protection (SIP):\n"
                                            "  disabled\n"
                                            "\n"
                                            "Advanced features (typing, read receipts):\n"
                                            "  Available - IMCore bridge connected\n"
                                            "  bridge version: v2 (v2 inbox active)\n"
                                            "  selectors:\n"
                                            "    editMessage: \xE2\x9C\x97\n"
                                            "    editMessageItem: \xE2\x9C\x97\n"
                                            "    pollPayloadMessage: \xE2\x9C\x93\n"
                                            "    retractMessagePart: \xE2\x9C\x93\n"
                                            "    sendMessageReason: \xE2\x9C\x97\n";

static void caps_selectors_deny_edit_but_allow_unsend_on_tahoe(void) {
    hu_imessage_caps_t caps;
    HU_ASSERT_EQ(
        hu_imessage_caps_parse(STATUS_BRIDGE_V2_TAHOE, strlen(STATUS_BRIDGE_V2_TAHOE), &caps),
        HU_OK);
    HU_ASSERT_TRUE(caps.advanced);
    HU_ASSERT_TRUE(caps.selectors_reported);
    /* The whole point: bridge live, yet edit must be DENIED. */
    HU_ASSERT_FALSE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_EDIT));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_UNSEND));
    /* Verbs not selector-gated still ride on `advanced`. */
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_REPLY_THREADED));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_TYPING));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_REACT));
}

static void caps_no_selector_section_falls_back_to_bridge_state(void) {
    /* Older imsg (or a future one that stops printing selectors): we cannot
     * know per-selector state, so edit/unsend follow the bridge flag rather
     * than being permanently disabled. */
    hu_imessage_caps_t caps;
    (void)hu_imessage_caps_parse(STATUS_BRIDGE_LIVE, strlen(STATUS_BRIDGE_LIVE), &caps);
    HU_ASSERT_TRUE(caps.advanced);
    HU_ASSERT_FALSE(caps.selectors_reported);
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_EDIT));
    HU_ASSERT_TRUE(hu_imessage_caps_allows(&caps, HU_IMSG_VERB_UNSEND));
}

/* ── T0.1 blue guard: never emit a green bubble ────────────────────────── */

static void blue_service_parse_from_chatdb_values(void) {
    /* Exact values present in chat.db on this box: iMessage / SMS / RCS. */
    HU_ASSERT_EQ((int)hu_imessage_service_from_string("iMessage", 8),
                 (int)HU_IMSG_SERVICE_IMESSAGE);
    HU_ASSERT_EQ((int)hu_imessage_service_from_string("SMS", 3), (int)HU_IMSG_SERVICE_SMS);
    HU_ASSERT_EQ((int)hu_imessage_service_from_string("RCS", 3), (int)HU_IMSG_SERVICE_RCS);
    HU_ASSERT_EQ((int)hu_imessage_service_from_string(NULL, 0), (int)HU_IMSG_SERVICE_UNKNOWN);
    HU_ASSERT_EQ((int)hu_imessage_service_from_string("carrier-pigeon", 14),
                 (int)HU_IMSG_SERVICE_UNKNOWN);
}

static void blue_only_imessage_is_blue(void) {
    HU_ASSERT_TRUE(hu_imessage_service_is_blue(HU_IMSG_SERVICE_IMESSAGE));
    /* RCS renders GREEN in Messages — it is not iMessage. */
    HU_ASSERT_FALSE(hu_imessage_service_is_blue(HU_IMSG_SERVICE_RCS));
    HU_ASSERT_FALSE(hu_imessage_service_is_blue(HU_IMSG_SERVICE_SMS));
    HU_ASSERT_FALSE(hu_imessage_service_is_blue(HU_IMSG_SERVICE_UNKNOWN));
}

static void blue_verdict_allows_proven_imessage(void) {
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_IMESSAGE, HU_IMSG_SERVICE_IMESSAGE),
                 (int)HU_BLUE_ALLOW);
    /* Most-recent message service wins over a stale handle row. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_IMESSAGE, HU_IMSG_SERVICE_SMS),
                 (int)HU_BLUE_ALLOW);
}

static void blue_verdict_holds_on_green_or_unknown(void) {
    /* Recent traffic went SMS → this contact is green right now. HOLD. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_SMS, HU_IMSG_SERVICE_IMESSAGE),
                 (int)HU_BLUE_HOLD);
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_RCS, HU_IMSG_SERVICE_RCS),
                 (int)HU_BLUE_HOLD);
    /* No signal at all → fail closed. "Blue no matter what" means we would
     * rather stay silent than send a green bubble. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_UNKNOWN, HU_IMSG_SERVICE_UNKNOWN),
                 (int)HU_BLUE_HOLD);
    /* Unknown recent, but the handle itself is an iMessage handle → ALLOW
     * (handle row is authoritative when there is no newer evidence). */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_UNKNOWN, HU_IMSG_SERVICE_IMESSAGE),
                 (int)HU_BLUE_ALLOW);
}

void run_imessage_caps_tests(void) {
    HU_TEST_SUITE("imessage_caps");
    HU_RUN_TEST(blue_service_parse_from_chatdb_values);
    HU_RUN_TEST(blue_only_imessage_is_blue);
    HU_RUN_TEST(blue_verdict_allows_proven_imessage);
    HU_RUN_TEST(blue_verdict_holds_on_green_or_unknown);
    HU_RUN_TEST(caps_parse_sip_on_gates_advanced);
    HU_RUN_TEST(caps_parse_bridge_live_enables_advanced);
    HU_RUN_TEST(caps_parse_empty_or_null_is_no_capability);
    HU_RUN_TEST(caps_parse_garbage_fails_closed);
    HU_RUN_TEST(caps_gate_basic_verbs_need_only_basic);
    HU_RUN_TEST(caps_gate_advanced_verbs_blocked_without_bridge);
    HU_RUN_TEST(caps_gate_advanced_verbs_allowed_with_bridge);
    HU_RUN_TEST(caps_gate_null_caps_denies_everything);
    HU_RUN_TEST(caps_describe_is_operator_readable);
    HU_RUN_TEST(caps_selectors_deny_edit_but_allow_unsend_on_tahoe);
    HU_RUN_TEST(caps_no_selector_section_falls_back_to_bridge_state);
}
