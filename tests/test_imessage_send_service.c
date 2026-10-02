/* Which service an outbound iMessage-channel send is addressed to.
 *
 * WHY (measured 2026-09-22): the send path hardcoded `--service imessage`,
 * which explicitly opts OUT of the imsg CLI's own iMessage->SMS fallback.
 * +1801xxx8303 is an RCS/Android contact with no iMessage account, so all 124
 * proactive check-ins aimed at them over 30 days delivered ZERO — while the two
 * contacts that DO have iMessage handles converted 11/12. The AppleScript
 * fallback has the same defect (`1st service whose service type = iMessage`).
 *
 * Pure (const char *) -> (const char *) so the decision is unit-tested without
 * a channel, a chat.db, or sending anything — same rationale as
 * hu_imessage_handle_excluded. */
#include "test_framework.h"

/* Gate-symmetry: src/channels/imessage.c is registered under if(HU_HAS_IMESSAGE)
 * (Apple-only), so a test calling its symbols unconditionally links on macOS and
 * fails on every Linux job. Stub runner in #else keeps the symbol resolvable.
 * See .claude/rules/test-source-gate-symmetry.md. */
#if HU_HAS_IMESSAGE
#include "human/channel_loop.h"
#include "human/channels/imessage.h"
#include "human/channels/imessage_send_route.h"
#include <stdlib.h>
#include <string.h>

#define ENVK "HU_IMESSAGE_SEND_SERVICE"

static void test_send_service_defaults_to_auto(void) {
    unsetenv(ENVK);
    /* "auto" is iMessage-first with an SMS fallback for text-only phone sends,
     * so a contact WITH iMessage is unaffected — this does not silently move
     * existing conversations to SMS. */
    HU_ASSERT_TRUE(strcmp(hu_imessage_send_service(), "auto") == 0);
}

static void test_send_service_env_can_restore_imessage_only(void) {
    /* The kill switch: restores the pre-2026-09-24 behaviour exactly, for an
     * operator who does not want any send leaving as SMS. */
    setenv(ENVK, "imessage", 1);
    HU_ASSERT_TRUE(strcmp(hu_imessage_send_service(), "imessage") == 0);
    unsetenv(ENVK);
}

static void test_send_service_env_can_force_sms(void) {
    setenv(ENVK, "sms", 1);
    HU_ASSERT_TRUE(strcmp(hu_imessage_send_service(), "sms") == 0);
    unsetenv(ENVK);
}

static void test_send_service_rejects_anything_else(void) {
    /* This value is spliced straight into the imsg argv. An unvalidated env
     * string would reach the CLI as a flag value — garbage in, failed send out,
     * and the operator sees only "send_failed". Unknown input must fall back to
     * the safe default, never pass through. */
    const char *junk[] = {"", "AUTO", "imessage ", "--no-sms-fallback", "sms;rm -rf /", "0", "x"};
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        setenv(ENVK, junk[i], 1);
        HU_ASSERT_TRUE(strcmp(hu_imessage_send_service(), "auto") == 0);
    }
    unsetenv(ENVK);
}

static void test_send_service_is_never_null_or_empty(void) {
    /* The caller puts this in argv unconditionally; a NULL would terminate the
     * array early and an empty string would make `--service` swallow the next
     * flag. */
    unsetenv(ENVK);
    const char *s = hu_imessage_send_service();
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(s[0] != '\0');
}

static void test_applescript_service_type_token(void) {
    /* The AppleScript fallback names its service inline:
     *   set targetService to 1st service whose service type = <TOKEN>
     * so the selected service must reach it too — otherwise
     * HU_IMESSAGE_SEND_SERVICE=sms would be silently ignored whenever the imsg
     * CLI is unavailable, which is a config that lies. */
    HU_ASSERT_TRUE(strcmp(hu_imessage_applescript_service_type("sms"), "SMS") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_applescript_service_type("imessage"), "iMessage") == 0);

    /* "auto" cannot be expressed in one tell block — a single `send` names
     * exactly one service, and an iMessage send to a non-iMessage buddy does
     * not reliably raise an AppleScript error to catch (it is accepted and then
     * never delivered). So auto degrades to iMessage here and the SMS fallback
     * only really works through the imsg CLI. Pinned so the degradation is a
     * documented choice, not an accident. */
    HU_ASSERT_TRUE(strcmp(hu_imessage_applescript_service_type("auto"), "iMessage") == 0);

    /* Unknown / NULL must not splice garbage into a script we execute. */
    HU_ASSERT_TRUE(strcmp(hu_imessage_applescript_service_type(NULL), "iMessage") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_applescript_service_type("nonsense"), "iMessage") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_applescript_service_type(""), "iMessage") == 0);
}

/* Wiring (2026-09-26 lost reply): the channel's poll path is what teaches
 * the send path which chat a contact wrote on. A 1:1 inbound on an SMS chat
 * must leave an SMS route behind; a group message must not (a group reply
 * goes to the group, never to one member's 1:1). */
static void poll_remembers_the_sms_chat_a_contact_wrote_on(void) {
    hu_imsg_route_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imsg_send_route_t r;
    HU_ASSERT_FALSE(hu_imsg_route_lookup("+15550003333", 12, &r));

    hu_imessage_test_msg_opts_t opts = {.chat_id = "SMS;-;+15550003333"};
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, "+15550003333", 12, "not much", 8, &opts),
                 HU_OK);
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);

    HU_ASSERT_TRUE(hu_imsg_route_lookup("+15550003333", 12, &r));
    HU_ASSERT_EQ((int)r.service, (int)HU_IMSG_SERVICE_SMS);
    HU_ASSERT_STR_EQ(r.chat_guid, "SMS;-;+15550003333");
    hu_imessage_destroy(&ch);
    hu_imsg_route_reset();
}

static void poll_group_message_leaves_no_route(void) {
    hu_imsg_route_reset();
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_msg_opts_t opts = {.chat_id = "SMS;+;chat42", .is_group = true};
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, "+15550004444", 12, "group hi", 8, &opts),
                 HU_OK);
    hu_channel_loop_msg_t msgs[2];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    hu_imsg_send_route_t r;
    HU_ASSERT_FALSE(hu_imsg_route_lookup("+15550004444", 12, &r));
    hu_imessage_destroy(&ch);
}

void run_imessage_send_service_tests(void) {
    HU_RUN_TEST(test_send_service_defaults_to_auto);
    HU_RUN_TEST(test_send_service_env_can_restore_imessage_only);
    HU_RUN_TEST(test_send_service_env_can_force_sms);
    HU_RUN_TEST(test_send_service_rejects_anything_else);
    HU_RUN_TEST(test_send_service_is_never_null_or_empty);
    HU_RUN_TEST(test_applescript_service_type_token);
    HU_RUN_TEST(poll_remembers_the_sms_chat_a_contact_wrote_on);
    HU_RUN_TEST(poll_group_message_leaves_no_route);
}

#else

void run_imessage_send_service_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE */
