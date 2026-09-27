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
#include "human/provider.h"
#include "human/tts/cartesia.h"
#include "test_framework.h"

#include <stdlib.h>
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
                                      reply, sizeof(reply) - 1, NULL, 0, 14);
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

/* F1 Task 4: the reply as written, before text shaping (typos, "haha "
 * prefixes). NULL = speak `reply`. */
static const char *g_unshaped;

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
                                 strlen(inbound), reply, strlen(reply), g_unshaped,
                                 g_unshaped ? strlen(g_unshaped) : 0, 14);
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

/* Final review #4: a memo cannot carry a link — the reply goes as text. */
static void test_voice_reply_with_link_goes_as_text(void) {
    g_voice_sends = 0;
    HU_ASSERT_FALSE(run_fallback_voice("this is the place https://example.com/a", "where?"));
    HU_ASSERT_FALSE(run_fallback_voice("https://example.com/a", "where?"));
    HU_ASSERT_EQ(g_voice_sends, 0);
}

static void test_voice_reply_nothing_speakable_goes_as_text(void) {
    g_voice_sends = 0;
    HU_ASSERT_FALSE(run_fallback_voice("\xF0\x9F\x91\x8D", "you around later?"));
    HU_ASSERT_EQ(g_voice_sends, 0);
}

/* F1 S4 through the daemon: with HU_SPEECH_REWRITE=live, a rewrite that
 * passes the drift guard but trips moderation must not be spoken. */
static const char *g_rewrite_out;
static int g_rewrite_calls;

static hu_error_t rw_mock_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sl,
                               const char *msg, size_t ml, const char *model, size_t mlen, double t,
                               char **out, size_t *out_len) {
    (void)ctx;
    (void)sys;
    (void)sl;
    (void)msg;
    (void)ml;
    (void)model;
    (void)mlen;
    (void)t;
    g_rewrite_calls++;
    size_t n = strlen(g_rewrite_out);
    char *b = alloc->alloc(alloc->ctx, n + 1);
    memcpy(b, g_rewrite_out, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static bool run_rewrite_voice_on(const char *reply, const char *rewrite, bool voice_enabled,
                                 const char *tts_provider) {
    static hu_provider_vtable_t pvt;
    memset(&pvt, 0, sizeof(pvt));
    pvt.chat_with_system = rw_mock_chat;
    g_rewrite_out = rewrite;
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.provider.vtable = &pvt;
    static hu_config_t config;
    memset(&config, 0, sizeof(config));
    config.channels.default_daemon.voice_enabled = voice_enabled;
    config.voice.tts_provider = (char *)tts_provider; /* test-owned literal, never freed */
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
    setenv("HU_SPEECH_REWRITE", "live", 1);
    bool sent = hu_daemon_voice_reply(&alloc, &agent, &config, &ch, "+15550000001", 12, "hey", 3,
                                      reply, strlen(reply), NULL, 0, 14);
    unsetenv("HU_SPEECH_REWRITE");
    return sent;
}

static bool run_rewrite_voice(const char *reply, const char *rewrite) {
    return run_rewrite_voice_on(reply, rewrite, true, "cartesia");
}

/* F2-voice direction (spec 2026-09-27): the model performs the line. */
static bool run_direct_voice(const char *reply, const char *model_line, const char *mode) {
    static hu_provider_vtable_t pvt;
    memset(&pvt, 0, sizeof(pvt));
    pvt.chat_with_system = rw_mock_chat;
    g_rewrite_out = model_line;
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.provider.vtable = &pvt;
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
    setenv("HU_SPEECH_DIRECTION", mode, 1);
    bool sent = hu_daemon_voice_reply(&alloc, &agent, &config, &ch, "+15550000001", 12, "hey", 3,
                                      reply, strlen(reply), NULL, 0, 14);
    unsetenv("HU_SPEECH_DIRECTION");
    return sent;
}

static void test_voice_reply_speaks_the_directed_line(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good",
                                    "<emotion value=\"excited\"/>Yeah, sounds good!", "live"));
    HU_ASSERT_EQ(g_voice_sends, 1);
    const char *t = hu_cartesia_test_last_transcript();
    HU_ASSERT_STR_CONTAINS(t, "<emotion value=\"excited\"/>");
    HU_ASSERT_STR_CONTAINS(t, "Yeah, sounds good!");
}

static void test_voice_reply_invalid_direction_speaks_plain_text(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good", "<prosody>Yeah</prosody>", "live"));
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "yeah sounds good");
}

static void test_voice_reply_direction_shadow_speaks_plain_text(void) {
    g_voice_sends = 0;
    g_rewrite_calls = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good",
                                    "<emotion value=\"excited\"/>Yeah, sounds good!", "shadow"));
    HU_ASSERT_EQ(g_rewrite_calls, 1);
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "yeah sounds good");
}

static void test_voice_reply_directed_line_that_trips_moderation_is_not_spoken(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_direct_voice("we watched that show with the kids",
                                    "<emotion value=\"calm\"/>kill them with violence and murder",
                                    "live"));
    const char *t = hu_cartesia_test_last_transcript();
    HU_ASSERT_STR_NOT_CONTAINS(t, "kill");
    HU_ASSERT_STR_CONTAINS(t, "we watched that show with the kids");
}

static void test_voice_reply_direction_off_is_todays_path(void) {
    g_voice_sends = 0;
    g_rewrite_calls = 0;
    HU_ASSERT_TRUE(
        run_direct_voice("yeah sounds good", "<emotion value=\"excited\"/>Yeah!", "off"));
    HU_ASSERT_EQ(g_rewrite_calls, 0);
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "yeah sounds good");
}

/* Final review #2: the rewrite is an LLM call; it runs only for a memo that
 * is actually going out, never for every text reply. */
static void test_voice_reply_rewrite_skipped_when_no_memo_can_go(void) {
    g_voice_sends = 0;
    g_rewrite_calls = 0;
    HU_ASSERT_FALSE(
        run_rewrite_voice_on("yeah sounds good", "Yeah, sounds good.", false, "cartesia"));
    HU_ASSERT_FALSE(run_rewrite_voice_on("yeah sounds good", "Yeah, sounds good.", true, NULL));
    HU_ASSERT_EQ(g_rewrite_calls, 0);
    HU_ASSERT_TRUE(run_rewrite_voice("yeah sounds good", "Yeah, sounds good."));
    HU_ASSERT_EQ(g_rewrite_calls, 1);
    HU_ASSERT_EQ(g_voice_sends, 1);
}

static void test_voice_reply_live_rewrite_is_spoken(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_rewrite_voice("yeah sounds good lmk", "Yeah, sounds good. Let me know."));
    HU_ASSERT_EQ(g_voice_sends, 1);
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "Yeah, sounds good. Let me know.");
}

static void test_voice_reply_rewrite_that_trips_moderation_is_not_spoken(void) {
    g_voice_sends = 0;
    HU_ASSERT_FALSE(run_rewrite_voice("we watched that show with the kids",
                                      "kill them with violence and murder"));
    HU_ASSERT_EQ(g_voice_sends, 0);
}

/* F1 Task 4: speak the reply as written — not the copy text shaping styled
 * for iMessage (injected typos, "haha " fillers). */
static void test_voice_reply_speaks_unshaped_reply(void) {
    g_voice_sends = 0;
    g_unshaped = "let me know when you're free";
    bool sent = run_fallback_voice("haha let me knwo when youre free", "you around later?");
    g_unshaped = NULL;
    HU_ASSERT_TRUE(sent);
    const char *spoken = hu_cartesia_test_last_transcript();
    HU_ASSERT_STR_EQ(spoken, "let me know when you're free");
}

static void test_voice_capture_unshaped_only_when_voice_possible(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_config_t config;
    memset(&config, 0, sizeof(config));
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = vr_name_generic;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.channel = &channel;
    size_t n = 99;
    HU_ASSERT_NULL(hu_daemon_voice_capture_unshaped(&alloc, &config, &ch, "hi there", 8, &n));
    HU_ASSERT_EQ(n, 0);
    config.channels.default_daemon.voice_enabled = true;
    char *copy = hu_daemon_voice_capture_unshaped(&alloc, &config, &ch, "hi there", 8, &n);
    HU_ASSERT_NOT_NULL(copy);
    HU_ASSERT_EQ(n, 8);
    HU_ASSERT_STR_EQ(copy, "hi there");
    alloc.free(alloc.ctx, copy, n + 1);
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
    HU_RUN_TEST(test_voice_reply_with_link_goes_as_text);
    HU_RUN_TEST(test_voice_reply_rewrite_skipped_when_no_memo_can_go);
    HU_RUN_TEST(test_voice_reply_speaks_the_directed_line);
    HU_RUN_TEST(test_voice_reply_invalid_direction_speaks_plain_text);
    HU_RUN_TEST(test_voice_reply_direction_shadow_speaks_plain_text);
    HU_RUN_TEST(test_voice_reply_directed_line_that_trips_moderation_is_not_spoken);
    HU_RUN_TEST(test_voice_reply_direction_off_is_todays_path);
    HU_RUN_TEST(test_voice_reply_speaks_unshaped_reply);
    HU_RUN_TEST(test_voice_capture_unshaped_only_when_voice_possible);
    HU_RUN_TEST(test_voice_reply_live_rewrite_is_spoken);
    HU_RUN_TEST(test_voice_reply_rewrite_that_trips_moderation_is_not_spoken);
    HU_RUN_TEST(test_voice_reply_flagged_reply_is_not_sent_as_voice);
    HU_RUN_TEST(test_voice_reply_inbound_crisis_is_not_sent_as_voice);
}
