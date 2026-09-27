/* tests/test_daemon_voice_reply.c
 *
 * hu_daemon_voice_reply — TTS decision + send carved out of hu_service_run
 * (src/daemon/daemon_voice_reply.c, 2026-09-12). Every voice send sits under
 * the channel's `voice_enabled` config; with no config the function must
 * report no voice sent and must not call the channel. The caller uses the
 * returned flag to decide whether the TEXT reply still goes out, so a false
 * `true` here would silently drop replies. */
#include "human/agent.h"
#include "human/daemon.h"
#include "test_framework.h"

#include <string.h>

static int g_voice_sends;

static const char *vr_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static hu_error_t vr_send(void *ctx, const char *to, size_t to_len, const char *text,
                          size_t text_len, const char *const *attachments,
                          size_t attachment_count) {
    (void)ctx;
    (void)to;
    (void)to_len;
    (void)text;
    (void)text_len;
    (void)attachments;
    (void)attachment_count;
    g_voice_sends++;
    return HU_OK;
}

static void test_voice_reply_without_config_sends_nothing_and_returns_false(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = vr_name;
    vt.send = vr_send;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.channel = &channel;

    static const char reply[] = "yeah call whenever";
    g_voice_sends = 0;
    bool sent = hu_daemon_voice_reply(&alloc, &agent, NULL, &ch, "+15550000001", 12, "hey", 3,
                                      reply, sizeof(reply) - 1, 14);
    HU_ASSERT_FALSE(sent);
    HU_ASSERT_EQ(g_voice_sends, 0);
}

/* Voice memos must pass the same outbound gates as text replies. Fixtures are
 * taken from the gate modules' own suites (test_moderation.c,
 * test_companion_safety.c, test_verify_claim.c). */
static void test_voice_gates_allow_clean_reply(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "hey how's your day going? did you finish that project?";
    const char *why = NULL;
    HU_ASSERT_TRUE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "clear");
}

static void test_voice_gates_decline_moderation_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "kill them with violence and murder";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "moderation");
}

static void test_voice_gates_decline_companion_safety_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "don't go, please stay, after everything we did you want to leave?";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "companion_safety");
}

static void test_voice_gates_decline_claim_language(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "I remember when you told me about your trip";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), &why));
    HU_ASSERT_STR_EQ(why, "claim_language");
}

static void test_voice_gates_fail_closed_on_invalid_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "invalid");
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, "", 0, NULL));
}

void run_daemon_voice_reply_tests(void) {
    HU_TEST_SUITE("daemon voice reply (carved)");
    HU_RUN_TEST(test_voice_reply_without_config_sends_nothing_and_returns_false);
    HU_RUN_TEST(test_voice_gates_allow_clean_reply);
    HU_RUN_TEST(test_voice_gates_decline_moderation_flag);
    HU_RUN_TEST(test_voice_gates_decline_companion_safety_flag);
    HU_RUN_TEST(test_voice_gates_decline_claim_language);
    HU_RUN_TEST(test_voice_gates_fail_closed_on_invalid_input);
}
