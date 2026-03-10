#ifndef HU_CHANNEL_CATALOG_H
#define HU_CHANNEL_CATALOG_H

#include "human/config.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_channel_id {
    HU_CHANNEL_CLI,
    HU_CHANNEL_TELEGRAM,
    HU_CHANNEL_DISCORD,
    HU_CHANNEL_SLACK,
    HU_CHANNEL_IMESSAGE,
    HU_CHANNEL_MATRIX,
    HU_CHANNEL_MATTERMOST,
    HU_CHANNEL_WHATSAPP,
    HU_CHANNEL_IRC,
    HU_CHANNEL_LARK,
    HU_CHANNEL_DINGTALK,
    HU_CHANNEL_SIGNAL,
    HU_CHANNEL_EMAIL,
    HU_CHANNEL_IMAP,
    HU_CHANNEL_LINE,
    HU_CHANNEL_QQ,
    HU_CHANNEL_ONEBOT,
    HU_CHANNEL_MAIXCAM,
    HU_CHANNEL_NOSTR,
    HU_CHANNEL_WEB,
    HU_CHANNEL_TEAMS,
    HU_CHANNEL_TWILIO,
    HU_CHANNEL_GOOGLE_CHAT,
    HU_CHANNEL_GMAIL,
    HU_CHANNEL_FACEBOOK,
    HU_CHANNEL_INSTAGRAM,
    HU_CHANNEL_TWITTER,
    HU_CHANNEL_GOOGLE_RCS,
    HU_CHANNEL_MQTT,
    HU_CHANNEL_DISPATCH,
    HU_CHANNEL_VOICE,
    HU_CHANNEL_COUNT
} hu_channel_id_t;

typedef enum hu_listener_mode {
    HU_LISTENER_NONE,
    HU_LISTENER_POLLING,
    HU_LISTENER_GATEWAY,
    HU_LISTENER_WEBHOOK_ONLY,
    HU_LISTENER_SEND_ONLY,
} hu_listener_mode_t;

typedef struct hu_channel_meta {
    hu_channel_id_t id;
    const char *key;
    const char *label;
    const char *configured_message;
    hu_listener_mode_t listener_mode;
} hu_channel_meta_t;

/* Get known channel metadata. */
const hu_channel_meta_t *hu_channel_catalog_all(size_t *out_count);

/* Check if channel is enabled in build (compile-time). */
bool hu_channel_catalog_is_build_enabled(hu_channel_id_t id);

/* Configured count for channel in config. */
size_t hu_channel_catalog_configured_count(const hu_config_t *cfg, hu_channel_id_t id);

/* Is channel configured (enabled in build and count > 0). */
bool hu_channel_catalog_is_configured(const hu_config_t *cfg, hu_channel_id_t id);

/* Status text for display. Caller provides buf of at least 64 bytes. */
const char *hu_channel_catalog_status_text(const hu_config_t *cfg, const hu_channel_meta_t *meta,
                                           char *buf, size_t buf_size);

/* Find meta by key. */
const hu_channel_meta_t *hu_channel_catalog_find_by_key(const char *key);

/* Has any non-CLI channel configured. */
bool hu_channel_catalog_has_any_configured(const hu_config_t *cfg, bool include_cli);

/* Does channel contribute to daemon supervision. */
bool hu_channel_catalog_contributes_to_daemon(hu_channel_id_t id);

/* Does channel require runtime (polling/gateway/webhook). */
bool hu_channel_catalog_requires_runtime(hu_channel_id_t id);

#endif /* HU_CHANNEL_CATALOG_H */
