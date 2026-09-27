/* tests/test_daemon_voice_reply.c
 *
 * hu_daemon_voice_reply — TTS decision + send carved out of hu_service_run
 * (src/daemon/daemon_voice_reply.c, 2026-09-12). Every voice send sits under
 * the channel's `voice_enabled` config; with no config the function must
 * report no voice sent and must not call the channel. The caller uses the
 * returned flag to decide whether the TEXT reply still goes out, so a false
 * `true` here would silently drop replies. */
#include "human/agent.h"
#include "human/config.h"
#include "human/daemon.h"
#include "human/tts/cartesia.h"
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
    HU_ASSERT_TRUE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "clear");
}

static void test_voice_gates_decline_moderation_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "kill them with violence and murder";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "moderation");
}

static void test_voice_gates_decline_companion_safety_flag(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "don't go, please stay, after everything we did you want to leave?";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "companion_safety");
}

static void test_voice_gates_decline_claim_language(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *t = "I remember when you told me about your trip";
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, t, strlen(t), NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "claim_language");
}

static void test_voice_gates_fail_closed_on_invalid_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *why = NULL;
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, NULL, 0, NULL, 0, &why));
    HU_ASSERT_STR_EQ(why, "invalid");
    HU_ASSERT_FALSE(hu_voice_reply_gates_clear(&alloc, "", 0, NULL, 0, NULL));
}

/* A person in crisis gets text — tappable resources, not a spoken memo — even
 * when the reply itself paraphrases the resources without trigger words. */
static void test_voice_gates_decline_when_inbound_is_crisis(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *reply = "i'm here for you. please reach out to 988 anytime, day or night";
    const char *inbound = "honestly i want to end it all";
    const char *why = NULL;
    HU_ASSERT_TRUE(hu_voice_reply_gates_clear(&alloc, reply, strlen(reply), NULL, 0, &why));
    HU_ASSERT_FALSE(
        hu_voice_reply_gates_clear(&alloc, reply, strlen(reply), inbound, strlen(inbound), &why));
    HU_ASSERT_STR_EQ(why, "inbound_crisis");
}

/* Daemon-level contract through the deterministic fallback arm (hu_voice_tts
 * mocks audio under HU_IS_TEST; a non-imessage channel writes a plain temp
 * file). The clean reply proves the send is reachable, so the flagged case
 * reaching zero sends is the gate — not an unreachable path. */
static const char *vr_name_generic(void *ctx) {
    (void)ctx;
    return "voicetest";
}

static bool run_fallback_voice(const char *reply, const char *inbound) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    static hu_config_t config;
    memset(&config, 0, sizeof(config));
    config.channels.default_daemon.voice_enabled = true;
    config.voice.tts_provider = "cartesia";
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = vr_name_generic;
    vt.send = vr_send;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.channel = &channel;
    return hu_daemon_voice_reply(&alloc, &agent, &config, &ch, "+15550000001", 12, inbound,
                                 strlen(inbound), reply, strlen(reply), 14);
}

/* F1 S2: the memo speaks the cleaned reply, not the texting shorthand. */
static void test_voice_reply_speaks_cleaned_text(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_fallback_voice("lmk when ur free lol", "you around later?"));
    HU_ASSERT_EQ(g_voice_sends, 1);
    const char *spoken = hu_cartesia_test_last_transcript();
    HU_ASSERT_NOT_NULL(spoken);
    HU_ASSERT_STR_CONTAINS(spoken, "let me know");
    HU_ASSERT_STR_NOT_CONTAINS(spoken, "lmk");
    HU_ASSERT_STR_NOT_CONTAINS(spoken, "lol");
}

static void test_voice_reply_nothing_speakable_goes_as_text(void) {
    g_voice_sends = 0;
    HU_ASSERT_FALSE(run_fallback_voice("\xF0\x9F\x91\x8D", "you around later?"));
    HU_ASSERT_EQ(g_voice_sends, 0);
}

static void test_voice_reply_sends_clean_reply_as_voice(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_fallback_voice("yeah call whenever, i'm around", "you free later?"));
    HU_ASSERT_EQ(g_voice_sends, 1);
}

static void test_voice_reply_flagged_reply_is_not_sent_as_voice(void) {
    g_voice_sends = 0;
    HU_ASSERT_FALSE(run_fallback_voice("kill them with violence and murder", "you free later?"));
    HU_ASSERT_EQ(g_voice_sends, 0);
}

static void test_voice_reply_inbound_crisis_is_not_sent_as_voice(void) {
    g_voice_sends = 0;
    HU_ASSERT_FALSE(
        run_fallback_voice("i'm here for you, call 988 anytime", "honestly i want to end it all"));
    HU_ASSERT_EQ(g_voice_sends, 0);
}

void run_daemon_voice_reply_tests(void) {
    HU_TEST_SUITE("daemon voice reply (carved)");
    HU_RUN_TEST(test_voice_reply_without_config_sends_nothing_and_returns_false);
    HU_RUN_TEST(test_voice_gates_allow_clean_reply);
    HU_RUN_TEST(test_voice_gates_decline_moderation_flag);
    HU_RUN_TEST(test_voice_gates_decline_companion_safety_flag);
    HU_RUN_TEST(test_voice_gates_decline_claim_language);
    HU_RUN_TEST(test_voice_gates_fail_closed_on_invalid_input);
    HU_RUN_TEST(test_voice_gates_decline_when_inbound_is_crisis);
    HU_RUN_TEST(test_voice_reply_sends_clean_reply_as_voice);
    HU_RUN_TEST(test_voice_reply_speaks_cleaned_text);
    HU_RUN_TEST(test_voice_reply_nothing_speakable_goes_as_text);
    HU_RUN_TEST(test_voice_reply_flagged_reply_is_not_sent_as_voice);
    HU_RUN_TEST(test_voice_reply_inbound_crisis_is_not_sent_as_voice);
}
