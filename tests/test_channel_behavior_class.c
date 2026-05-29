#include "human/channels/behavior_class.h"
#include "test_framework.h"
#include <string.h>

static int klass(const char *s) {
    return hu_channel_behavior_class_for_name(s, strlen(s));
}

static void behavior_class_maps_voice(void) {
    HU_ASSERT_EQ(klass("voice"), HU_CHANNEL_BEHAVIOR_VOICE);
}

static void behavior_class_maps_email_family(void) {
    HU_ASSERT_EQ(klass("email"), HU_CHANNEL_BEHAVIOR_EMAIL);
    HU_ASSERT_EQ(klass("imap"), HU_CHANNEL_BEHAVIOR_EMAIL);
    HU_ASSERT_EQ(klass("gmail"), HU_CHANNEL_BEHAVIOR_EMAIL);
}

static void behavior_class_maps_chat_family(void) {
    HU_ASSERT_EQ(klass("imessage"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("slack"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("telegram"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("discord"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("sms"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("whatsapp"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("mattermost"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("matrix"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("irc"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("line"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("lark"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("messenger"), HU_CHANNEL_BEHAVIOR_CHAT);
}

static void behavior_class_unknown_is_default(void) {
    HU_ASSERT_EQ(klass("carrier_pigeon"), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(hu_channel_behavior_class_for_name(NULL, 0), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(hu_channel_behavior_class_for_name("", 0), HU_CHANNEL_BEHAVIOR_DEFAULT);
}

/* Pins the latent prefix-collision bug the old memcmp had: a name that merely
 * STARTS WITH a known channel must NOT inherit its class. */
static void behavior_class_rejects_prefix_collision(void) {
    HU_ASSERT_EQ(klass("imessagebot"), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(klass("voicemail"), HU_CHANNEL_BEHAVIOR_DEFAULT);
    HU_ASSERT_EQ(klass("smsx"), HU_CHANNEL_BEHAVIOR_DEFAULT);
}

/* Case-insensitive: canonical names may arrive upper/mixed case. */
static void behavior_class_case_insensitive(void) {
    HU_ASSERT_EQ(klass("IMessage"), HU_CHANNEL_BEHAVIOR_CHAT);
    HU_ASSERT_EQ(klass("VOICE"), HU_CHANNEL_BEHAVIOR_VOICE);
}

void run_channel_behavior_class_tests(void) {
    HU_TEST_SUITE("channel_behavior_class");
    HU_RUN_TEST(behavior_class_maps_voice);
    HU_RUN_TEST(behavior_class_maps_email_family);
    HU_RUN_TEST(behavior_class_maps_chat_family);
    HU_RUN_TEST(behavior_class_unknown_is_default);
    HU_RUN_TEST(behavior_class_rejects_prefix_collision);
    HU_RUN_TEST(behavior_class_case_insensitive);
}
