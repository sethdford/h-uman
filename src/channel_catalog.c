#include "human/channel_catalog.h"
#include "human/config.h"
#include <string.h>

/* Static catalog for build-enabled channels.
 * Each entry: id, key (slug), label, configured_message, listener_mode.
 * configured_message is reserved for future use (e.g. webhook path hint). */
static const hu_channel_meta_t catalog[] = {
#ifdef HU_HAS_CLI
    {HU_CHANNEL_CLI, "cli", "CLI", "", HU_LISTENER_NONE},
#endif
#ifdef HU_HAS_TELEGRAM
    {HU_CHANNEL_TELEGRAM, "telegram", "Telegram", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_DISCORD
    {HU_CHANNEL_DISCORD, "discord", "Discord", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_SLACK
    {HU_CHANNEL_SLACK, "slack", "Slack", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_WHATSAPP
    {HU_CHANNEL_WHATSAPP, "whatsapp", "WhatsApp", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_MATRIX
    {HU_CHANNEL_MATRIX, "matrix", "Matrix", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_IRC
    {HU_CHANNEL_IRC, "irc", "IRC", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_LINE
    {HU_CHANNEL_LINE, "line", "LINE", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_LARK
    {HU_CHANNEL_LARK, "lark", "Lark", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_WEB
    {HU_CHANNEL_WEB, "web", "Web", "", HU_LISTENER_GATEWAY},
#endif
#ifdef HU_HAS_EMAIL
    {HU_CHANNEL_EMAIL, "email", "Email", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_IMAP
    {HU_CHANNEL_IMAP, "imap", "IMAP", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_IMESSAGE
    {HU_CHANNEL_IMESSAGE, "imessage", "iMessage", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_MATTERMOST
    {HU_CHANNEL_MATTERMOST, "mattermost", "Mattermost", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_ONEBOT
    {HU_CHANNEL_ONEBOT, "onebot", "OneBot", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_DINGTALK
    {HU_CHANNEL_DINGTALK, "dingtalk", "DingTalk", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_SIGNAL
    {HU_CHANNEL_SIGNAL, "signal", "Signal", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_NOSTR
    {HU_CHANNEL_NOSTR, "nostr", "Nostr", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_QQ
    {HU_CHANNEL_QQ, "qq", "QQ", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_MAIXCAM
    {HU_CHANNEL_MAIXCAM, "maixcam", "MaixCam", "", HU_LISTENER_SEND_ONLY},
#endif
#ifdef HU_HAS_TEAMS
    {HU_CHANNEL_TEAMS, "teams", "Microsoft Teams", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_TWILIO
    {HU_CHANNEL_TWILIO, "twilio", "Twilio SMS", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_GOOGLE_CHAT
    {HU_CHANNEL_GOOGLE_CHAT, "google_chat", "Google Chat", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_GMAIL
    {HU_CHANNEL_GMAIL, "gmail", "Gmail", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_FACEBOOK
    {HU_CHANNEL_FACEBOOK, "facebook", "Facebook Messenger", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_INSTAGRAM
    {HU_CHANNEL_INSTAGRAM, "instagram", "Instagram DMs", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_TWITTER
    {HU_CHANNEL_TWITTER, "twitter", "Twitter/X DMs", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_GOOGLE_RCS
    {HU_CHANNEL_GOOGLE_RCS, "google_rcs", "Google RCS", "", HU_LISTENER_POLLING},
#endif
#ifdef HU_HAS_MQTT
    {HU_CHANNEL_MQTT, "mqtt", "MQTT", "", HU_LISTENER_POLLING},
#endif
    {HU_CHANNEL_DISPATCH, "dispatch", "Dispatch", "", HU_LISTENER_NONE},
#ifdef HU_HAS_SONATA
    {HU_CHANNEL_VOICE, "voice", "Voice (Sonata)", "", HU_LISTENER_SEND_ONLY},
#endif
};
static const size_t catalog_len = sizeof(catalog) / sizeof(catalog[0]);

const hu_channel_meta_t *hu_channel_catalog_all(size_t *out_count) {
    *out_count = catalog_len;
    return catalog;
}

bool hu_channel_catalog_is_build_enabled(hu_channel_id_t id) {
    switch (id) {
#ifdef HU_HAS_CLI
    case HU_CHANNEL_CLI:
        return true;
#endif
#ifdef HU_HAS_TELEGRAM
    case HU_CHANNEL_TELEGRAM:
        return true;
#endif
#ifdef HU_HAS_DISCORD
    case HU_CHANNEL_DISCORD:
        return true;
#endif
#ifdef HU_HAS_SLACK
    case HU_CHANNEL_SLACK:
        return true;
#endif
#ifdef HU_HAS_WHATSAPP
    case HU_CHANNEL_WHATSAPP:
        return true;
#endif
#ifdef HU_HAS_MATRIX
    case HU_CHANNEL_MATRIX:
        return true;
#endif
#ifdef HU_HAS_IRC
    case HU_CHANNEL_IRC:
        return true;
#endif
#ifdef HU_HAS_LINE
    case HU_CHANNEL_LINE:
        return true;
#endif
#ifdef HU_HAS_LARK
    case HU_CHANNEL_LARK:
        return true;
#endif
#ifdef HU_HAS_WEB
    case HU_CHANNEL_WEB:
        return true;
#endif
#ifdef HU_HAS_EMAIL
    case HU_CHANNEL_EMAIL:
        return true;
#endif
#ifdef HU_HAS_IMAP
    case HU_CHANNEL_IMAP:
        return true;
#endif
#ifdef HU_HAS_IMESSAGE
    case HU_CHANNEL_IMESSAGE:
        return true;
#endif
#ifdef HU_HAS_MATTERMOST
    case HU_CHANNEL_MATTERMOST:
        return true;
#endif
#ifdef HU_HAS_ONEBOT
    case HU_CHANNEL_ONEBOT:
        return true;
#endif
#ifdef HU_HAS_DINGTALK
    case HU_CHANNEL_DINGTALK:
        return true;
#endif
#ifdef HU_HAS_SIGNAL
    case HU_CHANNEL_SIGNAL:
        return true;
#endif
#ifdef HU_HAS_NOSTR
    case HU_CHANNEL_NOSTR:
        return true;
#endif
#ifdef HU_HAS_QQ
    case HU_CHANNEL_QQ:
        return true;
#endif
#ifdef HU_HAS_MAIXCAM
    case HU_CHANNEL_MAIXCAM:
        return true;
#endif
#ifdef HU_HAS_TEAMS
    case HU_CHANNEL_TEAMS:
        return true;
#endif
#ifdef HU_HAS_TWILIO
    case HU_CHANNEL_TWILIO:
        return true;
#endif
#ifdef HU_HAS_GOOGLE_CHAT
    case HU_CHANNEL_GOOGLE_CHAT:
        return true;
#endif
#ifdef HU_HAS_GMAIL
    case HU_CHANNEL_GMAIL:
        return true;
#endif
#ifdef HU_HAS_FACEBOOK
    case HU_CHANNEL_FACEBOOK:
        return true;
#endif
#ifdef HU_HAS_INSTAGRAM
    case HU_CHANNEL_INSTAGRAM:
        return true;
#endif
#ifdef HU_HAS_TWITTER
    case HU_CHANNEL_TWITTER:
        return true;
#endif
#ifdef HU_HAS_GOOGLE_RCS
    case HU_CHANNEL_GOOGLE_RCS:
        return true;
#endif
#ifdef HU_HAS_MQTT
    case HU_CHANNEL_MQTT:
        return true;
#endif
    case HU_CHANNEL_DISPATCH:
        return true;
#ifdef HU_HAS_SONATA
    case HU_CHANNEL_VOICE:
        return true;
#endif
    default:
        return false;
    }
}

size_t hu_channel_catalog_configured_count(const hu_config_t *cfg, hu_channel_id_t id) {
    if (!cfg)
        return 0;
    if (id == HU_CHANNEL_CLI)
        return cfg->channels.cli ? 1 : 0;
    const hu_channel_meta_t *meta = NULL;
    for (size_t i = 0; i < catalog_len; i++) {
        if (catalog[i].id == id) {
            meta = &catalog[i];
            break;
        }
    }
    if (!meta || !meta->key)
        return 0;
    return hu_config_get_channel_configured_count(cfg, meta->key);
}

bool hu_channel_catalog_is_configured(const hu_config_t *cfg, hu_channel_id_t id) {
    return hu_channel_catalog_configured_count(cfg, id) > 0;
}

const char *hu_channel_catalog_status_text(const hu_config_t *cfg, const hu_channel_meta_t *meta,
                                           char *buf, size_t buf_size) {
    (void)cfg;
    if (buf_size > 0)
        buf[0] = '\0';
    return meta ? meta->key : buf;
}

const hu_channel_meta_t *hu_channel_catalog_find_by_key(const char *key) {
    if (!key)
        return NULL;
    size_t len = strlen(key);
    for (size_t i = 0; i < catalog_len; i++) {
        if (strncmp(catalog[i].key, key, len) == 0 && catalog[i].key[len] == '\0')
            return &catalog[i];
    }
    return NULL;
}

bool hu_channel_catalog_has_any_configured(const hu_config_t *cfg, bool include_cli) {
    if (!cfg)
        return false;
    if (include_cli && cfg->channels.cli)
        return true;
    for (size_t i = 0; i < cfg->channels.channel_config_len; i++) {
        if (cfg->channels.channel_config_keys[i] && cfg->channels.channel_config_counts[i] > 0)
            return true;
    }
    return false;
}

bool hu_channel_catalog_contributes_to_daemon(hu_channel_id_t id) {
    (void)id;
    return false;
}

bool hu_channel_catalog_requires_runtime(hu_channel_id_t id) {
    switch (id) {
    case HU_CHANNEL_TELEGRAM:
    case HU_CHANNEL_DISCORD:
    case HU_CHANNEL_SLACK:
    case HU_CHANNEL_WHATSAPP:
    case HU_CHANNEL_MATRIX:
    case HU_CHANNEL_IRC:
    case HU_CHANNEL_LINE:
    case HU_CHANNEL_LARK:
    case HU_CHANNEL_WEB:
    case HU_CHANNEL_MATTERMOST:
    case HU_CHANNEL_ONEBOT:
    case HU_CHANNEL_DINGTALK:
    case HU_CHANNEL_SIGNAL:
    case HU_CHANNEL_NOSTR:
    case HU_CHANNEL_QQ:
    case HU_CHANNEL_EMAIL:
    case HU_CHANNEL_IMAP:
    case HU_CHANNEL_IMESSAGE:
    case HU_CHANNEL_TEAMS:
    case HU_CHANNEL_TWILIO:
    case HU_CHANNEL_GOOGLE_CHAT:
    case HU_CHANNEL_GMAIL:
    case HU_CHANNEL_FACEBOOK:
    case HU_CHANNEL_INSTAGRAM:
    case HU_CHANNEL_TWITTER:
    case HU_CHANNEL_GOOGLE_RCS:
        return true;
    default:
        return false;
    }
}
