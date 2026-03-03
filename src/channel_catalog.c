#include "seaclaw/channel_catalog.h"
#include "seaclaw/config.h"
#include <string.h>

/* Static catalog for build-enabled channels.
 * Each entry: id, key (slug), label, configured_message, listener_mode.
 * configured_message is reserved for future use (e.g. webhook path hint). */
static const sc_channel_meta_t catalog[] = {
#ifdef SC_HAS_CLI
    { SC_CHANNEL_CLI, "cli", "CLI", "", SC_LISTENER_NONE },
#endif
#ifdef SC_HAS_TELEGRAM
    { SC_CHANNEL_TELEGRAM, "telegram", "Telegram", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_DISCORD
    { SC_CHANNEL_DISCORD, "discord", "Discord", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_SLACK
    { SC_CHANNEL_SLACK, "slack", "Slack", "", SC_LISTENER_GATEWAY },
#endif
#ifdef SC_HAS_WHATSAPP
    { SC_CHANNEL_WHATSAPP, "whatsapp", "WhatsApp", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_MATRIX
    { SC_CHANNEL_MATRIX, "matrix", "Matrix", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_IRC
    { SC_CHANNEL_IRC, "irc", "IRC", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_LINE
    { SC_CHANNEL_LINE, "line", "LINE", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_LARK
    { SC_CHANNEL_LARK, "lark", "Lark", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_WEB
    { SC_CHANNEL_WEB, "web", "Web", "", SC_LISTENER_GATEWAY },
#endif
#ifdef SC_HAS_EMAIL
    { SC_CHANNEL_EMAIL, "email", "Email", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_IMESSAGE
    { SC_CHANNEL_IMESSAGE, "imessage", "iMessage", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_MATTERMOST
    { SC_CHANNEL_MATTERMOST, "mattermost", "Mattermost", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_ONEBOT
    { SC_CHANNEL_ONEBOT, "onebot", "OneBot", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_DINGTALK
    { SC_CHANNEL_DINGTALK, "dingtalk", "DingTalk", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_SIGNAL
    { SC_CHANNEL_SIGNAL, "signal", "Signal", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_NOSTR
    { SC_CHANNEL_NOSTR, "nostr", "Nostr", "", SC_LISTENER_POLLING },
#endif
#ifdef SC_HAS_QQ
    { SC_CHANNEL_QQ, "qq", "QQ", "", SC_LISTENER_WEBHOOK_ONLY },
#endif
#ifdef SC_HAS_MAIXCAM
    { SC_CHANNEL_MAIXCAM, "maixcam", "MaixCam", "", SC_LISTENER_SEND_ONLY },
#endif
#ifdef SC_HAS_SONATA
    { SC_CHANNEL_VOICE, "voice", "Voice (Sonata)", "", SC_LISTENER_SEND_ONLY },
#endif
};
static const size_t catalog_len = sizeof(catalog) / sizeof(catalog[0]);

const sc_channel_meta_t *sc_channel_catalog_all(size_t *out_count) {
    *out_count = catalog_len;
    return catalog;
}

bool sc_channel_catalog_is_build_enabled(sc_channel_id_t id) {
    switch (id) {
#ifdef SC_HAS_CLI
    case SC_CHANNEL_CLI: return true;
#endif
#ifdef SC_HAS_TELEGRAM
    case SC_CHANNEL_TELEGRAM: return true;
#endif
#ifdef SC_HAS_DISCORD
    case SC_CHANNEL_DISCORD: return true;
#endif
#ifdef SC_HAS_SLACK
    case SC_CHANNEL_SLACK: return true;
#endif
#ifdef SC_HAS_WHATSAPP
    case SC_CHANNEL_WHATSAPP: return true;
#endif
#ifdef SC_HAS_MATRIX
    case SC_CHANNEL_MATRIX: return true;
#endif
#ifdef SC_HAS_IRC
    case SC_CHANNEL_IRC: return true;
#endif
#ifdef SC_HAS_LINE
    case SC_CHANNEL_LINE: return true;
#endif
#ifdef SC_HAS_LARK
    case SC_CHANNEL_LARK: return true;
#endif
#ifdef SC_HAS_WEB
    case SC_CHANNEL_WEB: return true;
#endif
#ifdef SC_HAS_EMAIL
    case SC_CHANNEL_EMAIL: return true;
#endif
#ifdef SC_HAS_IMESSAGE
    case SC_CHANNEL_IMESSAGE: return true;
#endif
#ifdef SC_HAS_MATTERMOST
    case SC_CHANNEL_MATTERMOST: return true;
#endif
#ifdef SC_HAS_ONEBOT
    case SC_CHANNEL_ONEBOT: return true;
#endif
#ifdef SC_HAS_DINGTALK
    case SC_CHANNEL_DINGTALK: return true;
#endif
#ifdef SC_HAS_SIGNAL
    case SC_CHANNEL_SIGNAL: return true;
#endif
#ifdef SC_HAS_NOSTR
    case SC_CHANNEL_NOSTR: return true;
#endif
#ifdef SC_HAS_QQ
    case SC_CHANNEL_QQ: return true;
#endif
#ifdef SC_HAS_MAIXCAM
    case SC_CHANNEL_MAIXCAM: return true;
#endif
#ifdef SC_HAS_SONATA
    case SC_CHANNEL_VOICE: return true;
#endif
    default: return false;
    }
}

size_t sc_channel_catalog_configured_count(const sc_config_t *cfg, sc_channel_id_t id) {
    if (!cfg) return 0;
    if (id == SC_CHANNEL_CLI)
        return cfg->channels.cli ? 1 : 0;
    const sc_channel_meta_t *meta = NULL;
    for (size_t i = 0; i < catalog_len; i++) {
        if (catalog[i].id == id) { meta = &catalog[i]; break; }
    }
    if (!meta || !meta->key) return 0;
    return sc_config_get_channel_configured_count(cfg, meta->key);
}

bool sc_channel_catalog_is_configured(const sc_config_t *cfg, sc_channel_id_t id) {
    return sc_channel_catalog_configured_count(cfg, id) > 0;
}

const char *sc_channel_catalog_status_text(const sc_config_t *cfg,
    const sc_channel_meta_t *meta, char *buf, size_t buf_size) {
    (void)cfg;
    if (buf_size > 0) buf[0] = '\0';
    return meta ? meta->key : buf;
}

const sc_channel_meta_t *sc_channel_catalog_find_by_key(const char *key) {
    if (!key) return NULL;
    size_t len = strlen(key);
    for (size_t i = 0; i < catalog_len; i++) {
        if (strncmp(catalog[i].key, key, len) == 0 && catalog[i].key[len] == '\0')
            return &catalog[i];
    }
    return NULL;
}

bool sc_channel_catalog_has_any_configured(const sc_config_t *cfg, bool include_cli) {
    if (!cfg) return false;
    if (include_cli && cfg->channels.cli) return true;
    for (size_t i = 0; i < cfg->channels.channel_config_len; i++) {
        if (cfg->channels.channel_config_keys[i] &&
            cfg->channels.channel_config_counts[i] > 0)
            return true;
    }
    return false;
}

bool sc_channel_catalog_contributes_to_daemon(sc_channel_id_t id) {
    (void)id;
    return false;
}

bool sc_channel_catalog_requires_runtime(sc_channel_id_t id) {
    switch (id) {
    case SC_CHANNEL_TELEGRAM:
    case SC_CHANNEL_DISCORD:
    case SC_CHANNEL_SLACK:
    case SC_CHANNEL_WHATSAPP:
    case SC_CHANNEL_MATRIX:
    case SC_CHANNEL_IRC:
    case SC_CHANNEL_LINE:
    case SC_CHANNEL_LARK:
    case SC_CHANNEL_WEB:
    case SC_CHANNEL_MATTERMOST:
    case SC_CHANNEL_ONEBOT:
    case SC_CHANNEL_DINGTALK:
    case SC_CHANNEL_SIGNAL:
    case SC_CHANNEL_NOSTR:
    case SC_CHANNEL_QQ:
    case SC_CHANNEL_EMAIL:
    case SC_CHANNEL_IMESSAGE:
        return true;
    default:
        return false;
    }
}
