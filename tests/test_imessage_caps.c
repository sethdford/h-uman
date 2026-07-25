/* Tests for the iMessage capability probe (T0.4) — the gate every native
 * verb routes through. Pins the `imsg status` parse (including the
 * "Not available" ⊃ "available" substring trap per
 * ~/.claude/rules/substring-classifier-pitfalls.md) and the verb gating
 * contract: nothing advanced is ever attempted without a proven bridge. */

#include "human/channels/imessage.h"
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

/* ── T0.1b live reachability (`imsg whois --json`) ──────────────────────
 * Every fixture below is real output captured from `imsg whois` on a Tahoe
 * 26.5.1 box with the IMCore bridge live (v2), 2026-07-19. Key ORDER is
 * preserved exactly as emitted — the tool varies it between identical calls,
 * which is precisely what the parser must tolerate. Only the phone numbers are
 * substituted (555 test ranges) so real contacts' numbers stay out of the repo;
 * the parser never reads the address field. */

/* Reachable on iMessage: recent iMessage traffic with this handle. */
static const char *WHOIS_REACHABLE =
    "{\"alias_type\":\"phone\",\"destination\":\"tel:+15550101234\",\"id_status\":1,"
    "\"available\":1,\"address\":\"+15550101234\"}\n";

/* Same verdict, DIFFERENT key order — captured from a later call for the very
 * same handle. Pins that lookup is by key, never by position. */
static const char *WHOIS_REACHABLE_REORDERED =
    "{\"available\":1,\"id_status\":1,\"address\":\"+15550101234\","
    "\"destination\":\"tel:+15550101234\",\"alias_type\":\"phone\"}\n";

/* SMS-only number — would render GREEN. */
static const char *WHOIS_SMS_ONLY =
    "{\"id_status\":0,\"address\":\"+15550102345\",\"available\":0,\"alias_type\":\"phone\","
    "\"destination\":\"tel:+15550102345\"}\n";

/* RCS handle. RCS also renders GREEN, and Apple reports it not-reachable. */
static const char *WHOIS_RCS =
    "{\"address\":\"+15550103456\",\"alias_type\":\"phone\",\"available\":0,\"id_status\":0,"
    "\"destination\":\"tel:+15550103456\"}\n";

/* Address that does not exist on iMessage at all. */
static const char *WHOIS_NONSENSE_EMAIL =
    "{\"address\":\"zzz-not-a-real-addr-9x7@example.invalid\",\"id_status\":0,"
    "\"alias_type\":\"email\",\"available\":0,"
    "\"destination\":\"mailto:zzz-not-a-real-addr-9x7@example.invalid\"}\n";

/* VERBATIM error output. Note: `imsg` prints this and STILL EXITS 0, so the
 * payload is the only usable signal. */
static const char *WHOIS_ERROR_TEXT = "Missing required option: --address\n";

static void whois_parse_reachable_says_reachable(void) {
    hu_whois_reach_t r = HU_WHOIS_NOT_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_REACHABLE, strlen(WHOIS_REACHABLE), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_REACHABLE);
}

static void whois_parse_is_key_order_independent(void) {
    hu_whois_reach_t r = HU_WHOIS_INDETERMINATE;
    HU_ASSERT_EQ(
        hu_imessage_whois_parse(WHOIS_REACHABLE_REORDERED, strlen(WHOIS_REACHABLE_REORDERED), &r),
        HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_REACHABLE);
}

/* The not-reachable case, across all three real shapes of it. */
static void whois_parse_green_handles_are_not_reachable(void) {
    hu_whois_reach_t r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_SMS_ONLY, strlen(WHOIS_SMS_ONLY), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_NOT_REACHABLE);

    r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_RCS, strlen(WHOIS_RCS), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_NOT_REACHABLE);

    r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_NONSENSE_EMAIL, strlen(WHOIS_NONSENSE_EMAIL), &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_NOT_REACHABLE);
}

/* Plain-text error, truncated JSON, empty, and garbage all mean "no answer" —
 * never "reachable". */
static void whois_parse_non_json_is_indeterminate(void) {
    hu_whois_reach_t r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_ERROR_TEXT, strlen(WHOIS_ERROR_TEXT), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_INDETERMINATE);

    /* Object, but the fields we require are absent. */
    const char *partial = "{\"address\":\"+15550101234\",\"alias_type\":\"phone\"}";
    r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(partial, strlen(partial), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_INDETERMINATE);

    /* Only one of the two required fields present → still no answer. */
    const char *half = "{\"available\":1,\"address\":\"+15550101234\"}";
    r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(half, strlen(half), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_INDETERMINATE);

    r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse("", 0, &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_INDETERMINATE);
    HU_ASSERT_EQ(hu_imessage_whois_parse(NULL, 0, &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_INDETERMINATE);
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_REACHABLE, strlen(WHOIS_REACHABLE), NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* The fields agreed in every observed probe, so disagreement is drift — and
 * drift must degrade to not-reachable, never to a green bubble. */
static void whois_parse_requires_both_fields_to_agree(void) {
    hu_whois_reach_t r = HU_WHOIS_INDETERMINATE;
    const char *split = "{\"id_status\":0,\"available\":1,\"alias_type\":\"phone\"}";
    HU_ASSERT_EQ(hu_imessage_whois_parse(split, strlen(split), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_NOT_REACHABLE);
}

/* `"status"` must NOT match inside `"id_status"`, and a key name appearing in a
 * string VALUE must not be read as that key. */
static void whois_parse_ignores_key_lookalikes(void) {
    hu_whois_reach_t r = HU_WHOIS_INDETERMINATE;
    const char *tricky = "{\"note\":\"available\",\"id_status\":1,\"available\":1}";
    HU_ASSERT_EQ(hu_imessage_whois_parse(tricky, strlen(tricky), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_REACHABLE);
}

/* A live POSITIVE is authoritative: it unblocks a handle chat.db knows nothing
 * about. That is the capability the chat.db-only guard could never have. */
static void whois_positive_allows_handle_with_no_history(void) {
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(HU_WHOIS_REACHABLE, HU_IMSG_SERVICE_UNKNOWN,
                                                    HU_IMSG_SERVICE_UNKNOWN, false),
                 (int)HU_BLUE_ALLOW);
    /* Pre-upgrade, that same handle was HELD — this is the behavior delta. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict(HU_IMSG_SERVICE_UNKNOWN, HU_IMSG_SERVICE_UNKNOWN),
                 (int)HU_BLUE_HOLD);
}

/* A live NEGATIVE is advisory by default — it has MEASURED false negatives
 * (2026-07-19: two active 1:1 iMessage threads answered available=0 on repeated
 * probes). It must NOT mute a thread chat.db proves is iMessage. */
static void whois_negative_is_advisory_and_does_not_mute_live_threads(void) {
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(HU_WHOIS_NOT_REACHABLE,
                                                    HU_IMSG_SERVICE_IMESSAGE,
                                                    HU_IMSG_SERVICE_IMESSAGE, false),
                 (int)HU_BLUE_ALLOW);
    /* …but it never RESCUES a green handle: chat.db still says SMS → HOLD. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(HU_WHOIS_NOT_REACHABLE, HU_IMSG_SERVICE_SMS,
                                                    HU_IMSG_SERVICE_SMS, false),
                 (int)HU_BLUE_HOLD);
}

/* Strict mode (HU_IMESSAGE_WHOIS_STRICT=1) makes the negative binding, for once
 * the false-negative rate has actually been measured. */
static void whois_negative_binds_in_strict_mode(void) {
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(HU_WHOIS_NOT_REACHABLE,
                                                    HU_IMSG_SERVICE_IMESSAGE,
                                                    HU_IMSG_SERVICE_IMESSAGE, true),
                 (int)HU_BLUE_HOLD);
    /* Strict mode must not disturb the positive or the indeterminate path. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(HU_WHOIS_REACHABLE, HU_IMSG_SERVICE_SMS,
                                                    HU_IMSG_SERVICE_SMS, true),
                 (int)HU_BLUE_ALLOW);
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(HU_WHOIS_INDETERMINATE, HU_IMSG_SERVICE_UNKNOWN,
                                                    HU_IMSG_SERVICE_UNKNOWN, true),
                 (int)HU_BLUE_HOLD);
}

/* ACCEPTANCE (b): a whois FAILURE must fall back to the chat.db verdict — and
 * that fallback must not become a licence to send. */
static void whois_failure_falls_back_to_chatdb_and_fails_closed(void) {
    /* Parse the real error text, then feed its verdict through the combiner —
     * end to end, no hand-written enum. */
    hu_whois_reach_t r = HU_WHOIS_REACHABLE;
    HU_ASSERT_EQ(hu_imessage_whois_parse(WHOIS_ERROR_TEXT, strlen(WHOIS_ERROR_TEXT), &r), HU_OK);
    HU_ASSERT_EQ((int)r, (int)HU_WHOIS_INDETERMINATE);

    /* No chat.db evidence either → HOLD, NOT allow. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(r, HU_IMSG_SERVICE_UNKNOWN,
                                                    HU_IMSG_SERVICE_UNKNOWN, false),
                 (int)HU_BLUE_HOLD);
    /* chat.db says green → HOLD. */
    HU_ASSERT_EQ(
        (int)hu_imessage_blue_verdict_live(r, HU_IMSG_SERVICE_SMS, HU_IMSG_SERVICE_IMESSAGE, false),
        (int)HU_BLUE_HOLD);
    HU_ASSERT_EQ(
        (int)hu_imessage_blue_verdict_live(r, HU_IMSG_SERVICE_RCS, HU_IMSG_SERVICE_RCS, false),
        (int)HU_BLUE_HOLD);
    /* chat.db proves iMessage → the pre-upgrade ALLOW is preserved, so a bridge
     * outage degrades to today's behavior instead of muting the daemon. */
    HU_ASSERT_EQ((int)hu_imessage_blue_verdict_live(r, HU_IMSG_SERVICE_IMESSAGE,
                                                    HU_IMSG_SERVICE_IMESSAGE, false),
                 (int)HU_BLUE_ALLOW);
}

/* In test builds the probe must never spawn a subprocess. */
static void whois_probe_never_spawns_under_test(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_allocator_t *alloc = &sys;
    const char *h = "+15550101234";
    HU_ASSERT_EQ((int)hu_imessage_whois_probe_cached(alloc, h, strlen(h)),
                 (int)HU_WHOIS_INDETERMINATE);
    /* Guard rails: NULL/empty/oversized handles are rejected outright. */
    HU_ASSERT_EQ((int)hu_imessage_whois_probe_cached(alloc, NULL, 0), (int)HU_WHOIS_INDETERMINATE);
    HU_ASSERT_EQ((int)hu_imessage_whois_probe_cached(NULL, h, strlen(h)),
                 (int)HU_WHOIS_INDETERMINATE);
}

void run_imessage_caps_tests(void) {
    HU_TEST_SUITE("imessage_caps");
    HU_RUN_TEST(whois_parse_reachable_says_reachable);
    HU_RUN_TEST(whois_parse_is_key_order_independent);
    HU_RUN_TEST(whois_parse_green_handles_are_not_reachable);
    HU_RUN_TEST(whois_parse_non_json_is_indeterminate);
    HU_RUN_TEST(whois_parse_requires_both_fields_to_agree);
    HU_RUN_TEST(whois_parse_ignores_key_lookalikes);
    HU_RUN_TEST(whois_positive_allows_handle_with_no_history);
    HU_RUN_TEST(whois_negative_is_advisory_and_does_not_mute_live_threads);
    HU_RUN_TEST(whois_negative_binds_in_strict_mode);
    HU_RUN_TEST(whois_failure_falls_back_to_chatdb_and_fails_closed);
    HU_RUN_TEST(whois_probe_never_spawns_under_test);
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

/* ── Emoji → native tapback kind (production bug 2026-07-20) ──────────────
 * The daemon's TAPBACK reply style called react_emoji, whose Tier-2 fallback
 * was a STUB returning NOT_SUPPORTED. Live log: "tapback emoji sent"=0 vs
 * "tapback unavailable, flat fallback"=29 — so every intended reaction was
 * sent as a PLAIN TEXT MESSAGE containing an emoji, which reads as fake.
 * With the IMCore bridge live, emoji must map to a real tapback kind. */

#ifdef HU_HAS_IMESSAGE
/* hu_imessage_reaction_for_emoji lives in src/channels/imessage.c, which is
 * compiled only under HU_HAS_IMESSAGE — gate the tests to match
 * (test-source-gate-symmetry.md pattern 2) or every non-mac CI variant
 * fails at link time. */

static void emoji_maps_to_native_reaction_kind(void) {
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("❤️"), (int)HU_REACTION_HEART);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("🙏"), (int)HU_REACTION_HEART);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("👍"), (int)HU_REACTION_THUMBS_UP);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("👎"), (int)HU_REACTION_THUMBS_DOWN);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("😂"), (int)HU_REACTION_HAHA);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("🤣"), (int)HU_REACTION_HAHA);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("🔥"), (int)HU_REACTION_EMPHASIS);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("🤔"), (int)HU_REACTION_QUESTION);
    /* Skin-tone variants must not fall through to the default. */
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("👍🏽"), (int)HU_REACTION_THUMBS_UP);
}

static void emoji_unknown_defaults_to_thumbs_up_never_none(void) {
    /* Must never return NONE: a NONE would re-open the text-fallback path
     * that produced the fake emoji messages. */
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji("🦖"), (int)HU_REACTION_THUMBS_UP);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji(""), (int)HU_REACTION_THUMBS_UP);
    HU_ASSERT_EQ((int)hu_imessage_reaction_for_emoji(NULL), (int)HU_REACTION_THUMBS_UP);
}

void run_imessage_emoji_reaction_tests(void) {
    HU_TEST_SUITE("imessage_emoji_reaction");
    HU_RUN_TEST(emoji_maps_to_native_reaction_kind);
    HU_RUN_TEST(emoji_unknown_defaults_to_thumbs_up_never_none);
}

#else /* !HU_HAS_IMESSAGE */

void run_imessage_emoji_reaction_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE */
