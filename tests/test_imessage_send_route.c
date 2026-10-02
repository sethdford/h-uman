/* Reply routing for green-bubble (SMS / RCS) chats.
 *
 * 2026-09-26: a reply to an RCS contact was generated and lost. imsg was
 * addressed `--to <handle> --service auto` and timed out; the AppleScript
 * fallback addressed `buddy <handle> of (1st service whose service type =
 * iMessage)`, which can never reach an RCS phone; no outcome was logged.
 *
 * These tests pin: (1) SMS/RCS chats are addressed by the chat the inbound
 * arrived on; (2) iMessage / unknown chats keep the exact pre-fix argv and
 * script; (3) every attempt logs one aggregate outcome line with no text and
 * no handle; (4) the send orchestration over a fake backend. No process is
 * spawned and nothing is sent. */
#include "human/channels/imessage_send_route.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

#define H     "+15550001111"
#define H_LEN 12

/* ── route memory ─────────────────────────────────────────────────────── */

static void route_unknown_handle_has_no_route(void) {
    hu_imsg_route_reset();
    hu_imsg_send_route_t r;
    HU_ASSERT_FALSE(hu_imsg_route_lookup(H, H_LEN, &r));
}

static void route_sms_inbound_is_remembered_with_service(void) {
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "SMS;-;" H, "SMS");
    hu_imsg_send_route_t r;
    HU_ASSERT_TRUE(hu_imsg_route_lookup(H, H_LEN, &r));
    HU_ASSERT_STR_EQ(r.chat_guid, "SMS;-;" H);
    HU_ASSERT_EQ((int)r.service, (int)HU_IMSG_SERVICE_SMS);
    HU_ASSERT_TRUE(hu_imsg_route_by_chat(&r));
}

static void route_macos26_any_guid_takes_service_from_message(void) {
    /* macOS 26 chat GUIDs start "any;-;" and say nothing about the service;
     * the inbound message's service column is the evidence. */
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "any;-;" H, "RCS");
    hu_imsg_send_route_t r;
    HU_ASSERT_TRUE(hu_imsg_route_lookup(H, H_LEN, &r));
    HU_ASSERT_EQ((int)r.service, (int)HU_IMSG_SERVICE_RCS);
    HU_ASSERT_TRUE(hu_imsg_route_by_chat(&r));
}

static void route_guid_prefix_used_when_service_missing(void) {
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "RCS;-;" H, NULL);
    hu_imsg_send_route_t r;
    HU_ASSERT_TRUE(hu_imsg_route_lookup(H, H_LEN, &r));
    HU_ASSERT_EQ((int)r.service, (int)HU_IMSG_SERVICE_RCS);
}

static void route_imessage_chat_is_not_addressed_by_chat(void) {
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "iMessage;-;" H, "iMessage");
    hu_imsg_send_route_t r;
    HU_ASSERT_TRUE(hu_imsg_route_lookup(H, H_LEN, &r));
    HU_ASSERT_FALSE(hu_imsg_route_by_chat(&r));
}

static void route_latest_inbound_wins(void) {
    /* A contact who moves from SMS to iMessage is answered on iMessage. */
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "any;-;" H, "SMS");
    hu_imsg_route_note_inbound(H, H_LEN, "any;-;" H, "iMessage");
    hu_imsg_send_route_t r;
    HU_ASSERT_TRUE(hu_imsg_route_lookup(H, H_LEN, &r));
    HU_ASSERT_EQ((int)r.service, (int)HU_IMSG_SERVICE_IMESSAGE);
}

static void route_rejects_guid_that_could_break_the_script(void) {
    /* The GUID is spliced into an AppleScript string: a quote must never be
     * stored, so it can never be executed. */
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "SMS;-;\" & do shell script \"x", "SMS");
    hu_imsg_send_route_t r;
    HU_ASSERT_FALSE(hu_imsg_route_lookup(H, H_LEN, &r));
}

static void route_handles_are_matched_exactly(void) {
    hu_imsg_route_reset();
    hu_imsg_route_note_inbound(H, H_LEN, "SMS;-;" H, "SMS");
    hu_imsg_send_route_t r;
    HU_ASSERT_FALSE(hu_imsg_route_lookup("+15550001112", 12, &r));
    HU_ASSERT_FALSE(hu_imsg_route_lookup(H, H_LEN - 1, &r));
}

/* ── argv / script building ───────────────────────────────────────────── */

static void argv_no_route_is_byte_identical_to_pre_fix(void) {
    const char *argv[12];
    size_t n = hu_imsg_route_build_argv(NULL, H, "hey", "auto", argv, 12);
    HU_ASSERT_EQ(n, 8u);
    const char *want[] = {"imsg", "send", "--to", H, "--text", "hey", "--service", "auto"};
    for (size_t i = 0; i < 8; i++)
        HU_ASSERT_STR_EQ(argv[i], want[i]);
    HU_ASSERT_NULL(argv[8]);
}

static void argv_imessage_route_is_byte_identical_to_pre_fix(void) {
    hu_imsg_send_route_t r = {.chat_guid = "iMessage;-;" H, .service = HU_IMSG_SERVICE_IMESSAGE};
    const char *argv[12];
    size_t n = hu_imsg_route_build_argv(&r, H, "hey", "auto", argv, 12);
    HU_ASSERT_EQ(n, 8u);
    HU_ASSERT_STR_EQ(argv[2], "--to");
    HU_ASSERT_STR_EQ(argv[3], H);
    HU_ASSERT_STR_EQ(argv[7], "auto");
}

static void argv_rcs_route_addresses_the_chat(void) {
    hu_imsg_send_route_t r = {.chat_guid = "any;-;" H, .service = HU_IMSG_SERVICE_RCS};
    const char *argv[12];
    size_t n = hu_imsg_route_build_argv(&r, H, "hey", "auto", argv, 12);
    HU_ASSERT_EQ(n, 6u);
    const char *want[] = {"imsg", "send", "--chat-guid", "any;-;" H, "--text", "hey"};
    for (size_t i = 0; i < 6; i++)
        HU_ASSERT_STR_EQ(argv[i], want[i]);
    HU_ASSERT_NULL(argv[6]);
}

static void argv_refuses_a_too_small_buffer(void) {
    const char *argv[4];
    HU_ASSERT_EQ(hu_imsg_route_build_argv(NULL, H, "hey", "auto", argv, 4), 0u);
}

static void script_imessage_is_byte_identical_to_pre_fix(void) {
    char got[512];
    int n = hu_imsg_route_build_applescript(NULL, "iMessage", H, "hey", got, sizeof(got));
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(got));
    HU_ASSERT_STR_EQ(got, "tell application \"Messages\"\n"
                          "  set targetService to 1st service whose service type = iMessage\n"
                          "  set targetBuddy to buddy \"" H "\" of targetService\n"
                          "  send \"hey\" to targetBuddy\n"
                          "end tell");
}

static void script_sms_route_sends_to_the_chat_not_an_imessage_buddy(void) {
    hu_imsg_send_route_t r = {.chat_guid = "SMS;-;" H, .service = HU_IMSG_SERVICE_SMS};
    char got[512];
    int n = hu_imsg_route_build_applescript(&r, "iMessage", H, "hey", got, sizeof(got));
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(got));
    HU_ASSERT_NOT_NULL(strstr(got, "chat id \"SMS;-;" H "\""));
    HU_ASSERT_NOT_NULL(strstr(got, "send \"hey\" to targetChat"));
    HU_ASSERT_NULL(strstr(got, "buddy"));
    HU_ASSERT_NULL(strstr(got, "service type = iMessage"));
    /* Bounded: a wedged Messages must not hold the daemon for 120 s. */
    HU_ASSERT_NOT_NULL(strstr(got, "with timeout of 20 seconds"));
}

/* ── outcome line ─────────────────────────────────────────────────────── */

static void outcome_line_is_aggregate_only(void) {
    hu_imsg_send_route_t r = {.chat_guid = "any;-;" H, .service = HU_IMSG_SERVICE_RCS};
    char line[160];
    hu_imsg_send_outcome_format(line, sizeof(line), "applescript", false, &r);
    HU_ASSERT_STR_EQ(line, "[send] path=applescript result=fail chat_service=RCS by_chat=1");
    HU_ASSERT_NULL(strstr(line, "5550001111"));
    hu_imsg_send_outcome_format(line, sizeof(line), "imsg", true, NULL);
    HU_ASSERT_STR_EQ(line, "[send] path=imsg result=ok chat_service=unknown by_chat=0");
}

/* ── orchestration over a fake backend ────────────────────────────────── */

typedef struct {
    bool imsg_ok, as_ok;
    int imsg_calls, as_calls;
    char argv_flat[512];
    char script[1024];
    char lines[4][160];
    int nlines;
} fake_t;

static bool fake_imsg(void *ctx, const char *const *argv) {
    fake_t *f = (fake_t *)ctx;
    f->imsg_calls++;
    f->argv_flat[0] = '\0';
    for (size_t i = 0; argv[i]; i++) {
        strncat(f->argv_flat, argv[i], sizeof(f->argv_flat) - strlen(f->argv_flat) - 2);
        strncat(f->argv_flat, " ", sizeof(f->argv_flat) - strlen(f->argv_flat) - 1);
    }
    return f->imsg_ok;
}
static bool fake_as(void *ctx, const char *script) {
    fake_t *f = (fake_t *)ctx;
    f->as_calls++;
    snprintf(f->script, sizeof(f->script), "%s", script);
    return f->as_ok;
}
static void fake_log(void *ctx, const char *line) {
    fake_t *f = (fake_t *)ctx;
    if (f->nlines < 4)
        snprintf(f->lines[f->nlines++], sizeof(f->lines[0]), "%s", line);
}

static hu_imsg_send_backend_t fake_backend(fake_t *f, bool imsg_available) {
    hu_imsg_send_backend_t be = {.ctx = f,
                                 .imsg_available = imsg_available,
                                 .run_imsg = fake_imsg,
                                 .run_applescript = fake_as,
                                 .log_outcome = fake_log};
    return be;
}

static hu_imsg_send_request_t req_for(const hu_imsg_send_route_t *r) {
    hu_imsg_send_request_t q = {.route = r,
                                .to = H,
                                .text = "hey",
                                .service = "auto",
                                .as_service = "iMessage",
                                .tgt_esc = H,
                                .msg_esc = "hey"};
    return q;
}

static void send_rcs_imsg_fails_then_chat_script_delivers(void) {
    fake_t f;
    memset(&f, 0, sizeof(f));
    f.as_ok = true;
    hu_imsg_send_route_t r = {.chat_guid = "any;-;" H, .service = HU_IMSG_SERVICE_RCS};
    hu_imsg_send_backend_t be = fake_backend(&f, true);
    hu_imsg_send_request_t q = req_for(&r);
    HU_ASSERT_EQ((int)hu_imsg_send_text_via(&be, &q), (int)HU_IMSG_SEND_PATH_APPLESCRIPT);
    HU_ASSERT_STR_EQ(f.argv_flat, "imsg send --chat-guid any;-;" H " --text hey ");
    HU_ASSERT_NOT_NULL(strstr(f.script, "chat id \"any;-;" H "\""));
    HU_ASSERT_EQ(f.nlines, 2);
    HU_ASSERT_STR_EQ(f.lines[0], "[send] path=imsg result=fail chat_service=RCS by_chat=1");
    HU_ASSERT_STR_EQ(f.lines[1], "[send] path=applescript result=ok chat_service=RCS by_chat=1");
}

static void send_both_paths_fail_returns_none_and_logs_both(void) {
    fake_t f;
    memset(&f, 0, sizeof(f));
    hu_imsg_send_route_t r = {.chat_guid = "SMS;-;" H, .service = HU_IMSG_SERVICE_SMS};
    hu_imsg_send_backend_t be = fake_backend(&f, true);
    hu_imsg_send_request_t q = req_for(&r);
    HU_ASSERT_EQ((int)hu_imsg_send_text_via(&be, &q), (int)HU_IMSG_SEND_PATH_NONE);
    HU_ASSERT_EQ(f.imsg_calls, 1);
    HU_ASSERT_EQ(f.as_calls, 1);
    HU_ASSERT_EQ(f.nlines, 2);
    HU_ASSERT_NOT_NULL(strstr(f.lines[1], "path=applescript result=fail chat_service=SMS"));
}

static void send_imessage_ok_on_imsg_is_one_attempt_with_old_argv(void) {
    fake_t f;
    memset(&f, 0, sizeof(f));
    f.imsg_ok = true;
    hu_imsg_send_backend_t be = fake_backend(&f, true);
    hu_imsg_send_request_t q = req_for(NULL);
    HU_ASSERT_EQ((int)hu_imsg_send_text_via(&be, &q), (int)HU_IMSG_SEND_PATH_IMSG);
    HU_ASSERT_STR_EQ(f.argv_flat, "imsg send --to " H " --text hey --service auto ");
    HU_ASSERT_EQ(f.as_calls, 0);
    HU_ASSERT_EQ(f.nlines, 1);
    HU_ASSERT_STR_EQ(f.lines[0], "[send] path=imsg result=ok chat_service=unknown by_chat=0");
}

static void send_without_imsg_goes_straight_to_applescript(void) {
    fake_t f;
    memset(&f, 0, sizeof(f));
    f.as_ok = true;
    hu_imsg_send_backend_t be = fake_backend(&f, false);
    hu_imsg_send_request_t q = req_for(NULL);
    HU_ASSERT_EQ((int)hu_imsg_send_text_via(&be, &q), (int)HU_IMSG_SEND_PATH_APPLESCRIPT);
    HU_ASSERT_EQ(f.imsg_calls, 0);
    HU_ASSERT_NOT_NULL(strstr(f.script, "buddy \"" H "\" of targetService"));
    HU_ASSERT_EQ(f.nlines, 1);
}

void run_imessage_send_route_tests(void) {
    HU_TEST_SUITE("imessage_send_route");
    HU_RUN_TEST(route_unknown_handle_has_no_route);
    HU_RUN_TEST(route_sms_inbound_is_remembered_with_service);
    HU_RUN_TEST(route_macos26_any_guid_takes_service_from_message);
    HU_RUN_TEST(route_guid_prefix_used_when_service_missing);
    HU_RUN_TEST(route_imessage_chat_is_not_addressed_by_chat);
    HU_RUN_TEST(route_latest_inbound_wins);
    HU_RUN_TEST(route_rejects_guid_that_could_break_the_script);
    HU_RUN_TEST(route_handles_are_matched_exactly);
    HU_RUN_TEST(argv_no_route_is_byte_identical_to_pre_fix);
    HU_RUN_TEST(argv_imessage_route_is_byte_identical_to_pre_fix);
    HU_RUN_TEST(argv_rcs_route_addresses_the_chat);
    HU_RUN_TEST(argv_refuses_a_too_small_buffer);
    HU_RUN_TEST(script_imessage_is_byte_identical_to_pre_fix);
    HU_RUN_TEST(script_sms_route_sends_to_the_chat_not_an_imessage_buddy);
    HU_RUN_TEST(outcome_line_is_aggregate_only);
    HU_RUN_TEST(send_rcs_imsg_fails_then_chat_script_delivers);
    HU_RUN_TEST(send_both_paths_fail_returns_none_and_logs_both);
    HU_RUN_TEST(send_imessage_ok_on_imsg_is_one_attempt_with_old_argv);
    HU_RUN_TEST(send_without_imsg_goes_straight_to_applescript);
    hu_imsg_route_reset();
}
