#include "channel_harness.h"
#include <string.h>

#if HU_HAS_IMESSAGE
#include "human/channels/imessage.h"

static hu_error_t imessage_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_imessage_create(alloc, "+15551234567", 12, NULL, 0, out);
}
static void imessage_test_destroy(hu_channel_t *ch) {
    hu_imessage_destroy(ch);
}
#endif

#if HU_HAS_TELEGRAM
#include "human/channels/telegram.h"

static hu_error_t telegram_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_telegram_create(alloc, "test-token", 10, out);
}
static void telegram_test_destroy(hu_channel_t *ch) {
    hu_telegram_destroy(ch);
}
#endif

#if HU_HAS_DISCORD
#include "human/channels/discord.h"

static hu_error_t discord_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_discord_create(alloc, "test-token", 10, out);
}
static void discord_test_destroy(hu_channel_t *ch) {
    hu_discord_destroy(ch);
}
#endif

#if HU_HAS_SLACK
#include "human/channels/slack.h"

static hu_error_t slack_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_slack_create(alloc, "test-token", 10, out);
}
static void slack_test_destroy(hu_channel_t *ch) {
    hu_slack_destroy(ch);
}
#endif

#if HU_HAS_SIGNAL
#include "human/channels/signal.h"

static hu_error_t signal_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_signal_create(alloc, "http://localhost:8080", 19, "test-account", 12, out);
}
static void signal_test_destroy(hu_channel_t *ch) {
    hu_signal_destroy(ch);
}
#endif

#if HU_HAS_WEB
#include "human/channels/web.h"

static hu_error_t web_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_web_create(alloc, out);
}
static void web_test_destroy(hu_channel_t *ch) {
    hu_web_destroy(ch);
}
#endif

#if HU_HAS_MQTT
#include "human/channels/mqtt.h"

static hu_error_t mqtt_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_mqtt_create(alloc, "mqtt://localhost:1883", 20, "in", 2, "out", 3, NULL, 0, NULL, 0,
                          0, out);
}
static void mqtt_test_destroy(hu_channel_t *ch) {
    hu_allocator_t a = hu_system_allocator();
    hu_mqtt_destroy(ch, &a);
}
#endif

#if HU_HAS_EMAIL
#include "human/channels/email.h"

static hu_error_t email_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_email_create(alloc, "localhost", 9, 587, "test@example.com", 16, out);
}
static void email_test_destroy(hu_channel_t *ch) {
    hu_email_destroy(ch);
}
static hu_error_t email_inject_wrapper(hu_channel_t *ch, const char *session_key, size_t sk_len,
                                       const char *content, size_t c_len) {
    return hu_email_test_inject_mock_email(ch, session_key, sk_len, content, c_len);
}
static const char *email_get_last_wrapper(hu_channel_t *ch, size_t *out_len) {
    const char *s = hu_email_test_last_message(ch);
    if (out_len && s)
        *out_len = strlen(s);
    return s;
}
#endif

#if HU_HAS_NOSTR
#include "human/channels/nostr.h"

static hu_error_t nostr_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_nostr_create(alloc, "/tmp/nak", 7, "npub1test", 9, "wss://relay.example.com", 22,
                           "seckey", 6, out);
}
static void nostr_test_destroy(hu_channel_t *ch) {
    hu_nostr_destroy(ch);
}
static const char *nostr_get_last_wrapper(hu_channel_t *ch, size_t *out_len) {
    const char *s = hu_nostr_test_last_message(ch);
    if (out_len && s)
        *out_len = strlen(s);
    return s;
}
#endif

#if HU_HAS_QQ
#include "human/channels/qq.h"

static hu_error_t qq_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_qq_create(alloc, "test-app-id", 11, "test-bot-token", 14, false, out);
}
static void qq_test_destroy(hu_channel_t *ch) {
    hu_qq_destroy(ch);
}
#endif

#if HU_HAS_DINGTALK
#include "human/channels/dingtalk.h"

static hu_error_t dingtalk_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_dingtalk_create(alloc, "test-app-key", 12, "test-app-secret", 16, out);
}
static void dingtalk_test_destroy(hu_channel_t *ch) {
    hu_dingtalk_destroy(ch);
}
#endif

#if HU_HAS_LARK
#include "human/channels/lark.h"

static hu_error_t lark_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_lark_create(alloc, "test-app-id", 11, "test-app-secret", 16, out);
}
static void lark_test_destroy(hu_channel_t *ch) {
    hu_lark_destroy(ch);
}
#endif

#if HU_HAS_MATTERMOST
#include "human/channels/mattermost.h"

static hu_error_t mattermost_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_mattermost_create(alloc, "http://localhost:8065", 20, "test-token", 10, out);
}
static void mattermost_test_destroy(hu_channel_t *ch) {
    hu_mattermost_destroy(ch);
}
#endif

#if HU_HAS_TEAMS
#include "human/channels/teams.h"

static hu_error_t teams_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_teams_create(alloc, "https://example.com/webhook", 26, out);
}
static void teams_test_destroy(hu_channel_t *ch) {
    hu_teams_destroy(ch);
}
#endif

#if HU_HAS_TWILIO
#include "human/channels/twilio.h"

static hu_error_t twilio_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_twilio_create(alloc, "ACtest", 6, "test-auth-token", 15, "+15551234567", 12,
                            "+15559876543", 12, out);
}
static void twilio_test_destroy(hu_channel_t *ch) {
    hu_twilio_destroy(ch);
}
#endif

#if HU_HAS_GOOGLE_CHAT
#include "human/channels/google_chat.h"

static hu_error_t google_chat_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_google_chat_create(alloc, "https://example.com/webhook", 27, out);
}
static void google_chat_test_destroy(hu_channel_t *ch) {
    hu_google_chat_destroy(ch);
}
#endif

#if HU_HAS_GOOGLE_RCS
#include "human/channels/google_rcs.h"

static hu_error_t google_rcs_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_google_rcs_create(alloc, "test-agent-id", 13, "test-token", 10, out);
}
static void google_rcs_test_destroy(hu_channel_t *ch) {
    hu_google_rcs_destroy(ch);
}
#endif

#if HU_HAS_GMAIL
#include "human/channels/gmail.h"

static hu_error_t gmail_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_gmail_create(alloc, "test-client-id", 14, "test-client-secret", 18,
                           "test-refresh-token", 17, 60, out);
}
static void gmail_test_destroy(hu_channel_t *ch) {
    hu_gmail_destroy(ch);
}
#endif

#if HU_HAS_LINE
#include "human/channels/line.h"

static hu_error_t line_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_line_create(alloc, "test-channel-token", 18, out);
}
static void line_test_destroy(hu_channel_t *ch) {
    hu_line_destroy(ch);
}
#endif

#if HU_HAS_ONEBOT
#include "human/channels/onebot.h"

static hu_error_t onebot_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_onebot_create(alloc, "http://localhost:5700", 19, "test-token", 10, out);
}
static void onebot_test_destroy(hu_channel_t *ch) {
    hu_onebot_destroy(ch);
}
#endif

#if HU_HAS_INSTAGRAM
#include "human/channels/instagram.h"

static hu_error_t instagram_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_instagram_create(alloc, "test-business-id", 15, "test-access-token", 17,
                               "test-app-secret", 15, out);
}
static void instagram_test_destroy(hu_channel_t *ch) {
    hu_instagram_destroy(ch);
}
#endif

#if HU_HAS_FACEBOOK
#include "human/channels/facebook.h"

static hu_error_t facebook_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_facebook_create(alloc, "test-page-id", 12, "test-token", 10, "test-secret", 11, out);
}
static void facebook_test_destroy(hu_channel_t *ch) {
    hu_facebook_destroy(ch);
}
#endif

#if HU_HAS_TWITTER
#include "human/channels/twitter.h"

static hu_error_t twitter_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_twitter_create(alloc, "test-bearer-token", 16, out);
}
static void twitter_test_destroy(hu_channel_t *ch) {
    hu_twitter_destroy(ch);
}
#endif

#if HU_HAS_MAIXCAM
#include "human/channels/maixcam.h"

static hu_error_t maixcam_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_maixcam_create(alloc, "localhost", 9, 8080, out);
}
static void maixcam_test_destroy(hu_channel_t *ch) {
    hu_maixcam_destroy(ch);
}
#endif

#if HU_HAS_MATRIX
#include "human/channels/matrix.h"

static hu_error_t matrix_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_matrix_create(alloc, "https://matrix.example.com", 25, "test-access-token", 17, out);
}
static void matrix_test_destroy(hu_channel_t *ch) {
    hu_matrix_destroy(ch);
}
#endif

#if HU_HAS_IRC
#include "human/channels/irc.h"

static hu_error_t irc_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_irc_create(alloc, "localhost", 9, 6667, out);
}
static void irc_test_destroy(hu_channel_t *ch) {
    hu_irc_destroy(ch);
}
#endif

#if HU_HAS_WHATSAPP
#include "human/channels/whatsapp.h"

static hu_error_t whatsapp_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_whatsapp_create(alloc, "test-phone-id", 12, "test-token", 10, out);
}
static void whatsapp_test_destroy(hu_channel_t *ch) {
    hu_whatsapp_destroy(ch);
}
#endif

#if HU_HAS_IMAP
#include "human/channels/imap.h"

static hu_error_t imap_test_create(hu_allocator_t *alloc, hu_channel_t *out) {
    hu_imap_config_t config = {0};
    config.imap_host = "localhost";
    config.imap_host_len = 9;
    config.imap_port = 143;
    config.imap_username = "test";
    config.imap_username_len = 4;
    config.imap_password = "test";
    config.imap_password_len = 4;
    config.imap_folder = "INBOX";
    config.imap_folder_len = 5;
    config.imap_use_tls = false;
    return hu_imap_create(alloc, &config, out);
}
static void imap_test_destroy(hu_channel_t *ch) {
    hu_imap_destroy(ch);
}
static hu_error_t imap_inject_wrapper(hu_channel_t *ch, const char *session_key, size_t sk_len,
                                      const char *content, size_t c_len) {
    return hu_imap_test_push_mock(ch, session_key, sk_len, content, c_len);
}
#endif

static const hu_channel_test_entry_t s_registry[] = {
#if HU_HAS_IMESSAGE
    {"imessage", imessage_test_create, imessage_test_destroy, hu_imessage_test_inject_mock,
     hu_imessage_poll, hu_imessage_test_get_last_message},
#endif
#if HU_HAS_TELEGRAM
    {"telegram", telegram_test_create, telegram_test_destroy, hu_telegram_test_inject_mock,
     hu_telegram_poll, hu_telegram_test_get_last_message},
#endif
#if HU_HAS_DISCORD
    {"discord", discord_test_create, discord_test_destroy, hu_discord_test_inject_mock,
     hu_discord_poll, hu_discord_test_get_last_message},
#endif
#if HU_HAS_SLACK
    {"slack", slack_test_create, slack_test_destroy, hu_slack_test_inject_mock, hu_slack_poll,
     hu_slack_test_get_last_message},
#endif
#if HU_HAS_SIGNAL
    {"signal", signal_test_create, signal_test_destroy, hu_signal_test_inject_mock, hu_signal_poll,
     hu_signal_test_get_last_message},
#endif
#if HU_HAS_WEB
    {"web", web_test_create, web_test_destroy, hu_web_test_inject_mock, NULL,
     hu_web_test_get_last_message},
#endif
#if HU_HAS_MQTT
    {"mqtt", mqtt_test_create, mqtt_test_destroy, hu_mqtt_test_inject_mock, hu_mqtt_poll,
     hu_mqtt_test_get_last_message},
#endif
#if HU_HAS_EMAIL
    {"email", email_test_create, email_test_destroy, email_inject_wrapper, hu_email_poll,
     email_get_last_wrapper},
#endif
#if HU_HAS_NOSTR
    {"nostr", nostr_test_create, nostr_test_destroy, hu_nostr_test_inject_mock_event, hu_nostr_poll,
     nostr_get_last_wrapper},
#endif
#if HU_HAS_QQ
    {"qq", qq_test_create, qq_test_destroy, hu_qq_test_inject_mock, hu_qq_poll,
     hu_qq_test_get_last_message},
#endif
#if HU_HAS_DINGTALK
    {"dingtalk", dingtalk_test_create, dingtalk_test_destroy, hu_dingtalk_test_inject_mock,
     hu_dingtalk_poll, hu_dingtalk_test_get_last_message},
#endif
#if HU_HAS_LARK
    {"lark", lark_test_create, lark_test_destroy, hu_lark_test_inject_mock, hu_lark_poll,
     hu_lark_test_get_last_message},
#endif
#if HU_HAS_MATTERMOST
    {"mattermost", mattermost_test_create, mattermost_test_destroy, hu_mattermost_test_inject_mock,
     hu_mattermost_poll, hu_mattermost_test_get_last_message},
#endif
#if HU_HAS_TEAMS
    {"teams", teams_test_create, teams_test_destroy, hu_teams_test_inject_mock, hu_teams_poll,
     hu_teams_test_get_last_message},
#endif
#if HU_HAS_TWILIO
    {"twilio", twilio_test_create, twilio_test_destroy, hu_twilio_test_inject_mock, hu_twilio_poll,
     hu_twilio_test_get_last_message},
#endif
#if HU_HAS_GOOGLE_CHAT
    {"google_chat", google_chat_test_create, google_chat_test_destroy,
     hu_google_chat_test_inject_mock, hu_google_chat_poll, hu_google_chat_test_get_last_message},
#endif
#if HU_HAS_GOOGLE_RCS
    {"google_rcs", google_rcs_test_create, google_rcs_test_destroy, hu_google_rcs_test_inject_mock,
     hu_google_rcs_poll, hu_google_rcs_test_get_last_message},
#endif
#if HU_HAS_GMAIL
    {"gmail", gmail_test_create, gmail_test_destroy, hu_gmail_test_inject_mock, hu_gmail_poll,
     hu_gmail_test_get_last_message},
#endif
#if HU_HAS_LINE
    {"line", line_test_create, line_test_destroy, hu_line_test_inject_mock, hu_line_poll,
     hu_line_test_get_last_message},
#endif
#if HU_HAS_ONEBOT
    {"onebot", onebot_test_create, onebot_test_destroy, hu_onebot_test_inject_mock, hu_onebot_poll,
     hu_onebot_test_get_last_message},
#endif
#if HU_HAS_INSTAGRAM
    {"instagram", instagram_test_create, instagram_test_destroy, hu_instagram_test_inject_mock,
     hu_instagram_poll, hu_instagram_test_get_last_message},
#endif
#if HU_HAS_FACEBOOK
    {"facebook", facebook_test_create, facebook_test_destroy, hu_facebook_test_inject_mock,
     hu_facebook_poll, hu_facebook_test_get_last_message},
#endif
#if HU_HAS_TWITTER
    {"twitter", twitter_test_create, twitter_test_destroy, hu_twitter_test_inject_mock,
     hu_twitter_poll, hu_twitter_test_get_last_message},
#endif
#if HU_HAS_MAIXCAM
    {"maixcam", maixcam_test_create, maixcam_test_destroy, hu_maixcam_test_inject_mock, NULL,
     hu_maixcam_test_get_last_message},
#endif
#if HU_HAS_MATRIX
    {"matrix", matrix_test_create, matrix_test_destroy, hu_matrix_test_inject_mock, hu_matrix_poll,
     hu_matrix_test_get_last_message},
#endif
#if HU_HAS_IRC
    {"irc", irc_test_create, irc_test_destroy, hu_irc_test_inject_mock, hu_irc_poll,
     hu_irc_test_get_last_message},
#endif
#if HU_HAS_WHATSAPP
    {"whatsapp", whatsapp_test_create, whatsapp_test_destroy, hu_whatsapp_test_inject_mock,
     hu_whatsapp_poll, hu_whatsapp_test_get_last_message},
#endif
#if HU_HAS_IMAP
    {"imap", imap_test_create, imap_test_destroy, imap_inject_wrapper, hu_imap_poll, NULL},
#endif
};

const hu_channel_test_entry_t *hu_channel_test_registry(size_t *count) {
    *count = sizeof(s_registry) / sizeof(s_registry[0]);
    return s_registry;
}

const hu_channel_test_entry_t *hu_channel_test_find(const char *name) {
    size_t n;
    const hu_channel_test_entry_t *r = hu_channel_test_registry(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(r[i].name, name) == 0)
            return &r[i];
    return NULL;
}
