#ifndef SC_CHANNEL_CATALOG_H
#define SC_CHANNEL_CATALOG_H

#include "seaclaw/config.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum sc_channel_id {
    SC_CHANNEL_CLI,
    SC_CHANNEL_TELEGRAM,
    SC_CHANNEL_DISCORD,
    SC_CHANNEL_SLACK,
    SC_CHANNEL_WEBHOOK,
    SC_CHANNEL_IMESSAGE,
    SC_CHANNEL_MATRIX,
    SC_CHANNEL_MATTERMOST,
    SC_CHANNEL_WHATSAPP,
    SC_CHANNEL_IRC,
    SC_CHANNEL_LARK,
    SC_CHANNEL_DINGTALK,
    SC_CHANNEL_SIGNAL,
    SC_CHANNEL_EMAIL,
    SC_CHANNEL_LINE,
    SC_CHANNEL_QQ,
    SC_CHANNEL_ONEBOT,
    SC_CHANNEL_MAIXCAM,
    SC_CHANNEL_NOSTR,
    SC_CHANNEL_WEB,
    SC_CHANNEL_COUNT
} sc_channel_id_t;

typedef enum sc_listener_mode {
    SC_LISTENER_NONE,
    SC_LISTENER_POLLING,
    SC_LISTENER_GATEWAY,
    SC_LISTENER_WEBHOOK_ONLY,
    SC_LISTENER_SEND_ONLY,
} sc_listener_mode_t;

typedef struct sc_channel_meta {
    sc_channel_id_t id;
    const char *key;
    const char *label;
    const char *configured_message;
    sc_listener_mode_t listener_mode;
} sc_channel_meta_t;

/* Get known channel metadata. */
const sc_channel_meta_t *sc_channel_catalog_all(size_t *out_count);

/* Check if channel is enabled in build (compile-time). */
bool sc_channel_catalog_is_build_enabled(sc_channel_id_t id);

/* Configured count for channel in config. */
size_t sc_channel_catalog_configured_count(const sc_config_t *cfg, sc_channel_id_t id);

/* Is channel configured (enabled in build and count > 0). */
bool sc_channel_catalog_is_configured(const sc_config_t *cfg, sc_channel_id_t id);

/* Status text for display. Caller provides buf of at least 64 bytes. */
const char *sc_channel_catalog_status_text(const sc_config_t *cfg,
    const sc_channel_meta_t *meta, char *buf, size_t buf_size);

/* Find meta by key. */
const sc_channel_meta_t *sc_channel_catalog_find_by_key(const char *key);

/* Has any non-CLI channel configured. */
bool sc_channel_catalog_has_any_configured(const sc_config_t *cfg, bool include_cli);

/* Does channel contribute to daemon supervision. */
bool sc_channel_catalog_contributes_to_daemon(sc_channel_id_t id);

/* Does channel require runtime (polling/gateway/webhook). */
bool sc_channel_catalog_requires_runtime(sc_channel_id_t id);

#endif /* SC_CHANNEL_CATALOG_H */
