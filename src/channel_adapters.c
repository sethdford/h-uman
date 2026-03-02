#include "seaclaw/channel_adapters.h"
#include <string.h>

/* Minimal stub: parse peer kind for agent_routing compatibility. */
int sc_channel_adapters_parse_peer_kind(const char *raw, size_t len) {
    if (!raw) return -1;
    if (len >= 6 && strncmp(raw, "direct", 6) == 0) return (int)SC_CHAT_DIRECT;
    if (len >= 2 && strncmp(raw, "dm", 2) == 0) return (int)SC_CHAT_DIRECT;
    if (len >= 5 && strncmp(raw, "group", 5) == 0) return (int)SC_CHAT_GROUP;
    if (len >= 7 && strncmp(raw, "channel", 7) == 0) return (int)SC_CHAT_CHANNEL;
    return -1;
}

/* Polling descriptors for SC_LISTENER_POLLING channels (telegram, matrix, irc, signal, nostr). */
static const sc_polling_descriptor_t polling_table[] = {
    { "telegram", 1000 },
    { "discord", 2000 },
    { "matrix", 2000 },
    { "irc", 1000 },
    { "signal", 2000 },
    { "nostr", 5000 },
    { "imessage", 3000 },
    { "email", 30000 },
};
static const size_t polling_table_len = sizeof(polling_table) / sizeof(polling_table[0]);

const sc_polling_descriptor_t *sc_channel_adapters_find_polling_descriptor(const char *channel_name, size_t len) {
    if (!channel_name) return NULL;
    for (size_t i = 0; i < polling_table_len; i++) {
        const char *key = polling_table[i].channel_name;
        size_t key_len = strlen(key);
        if (key_len == len && strncmp(channel_name, key, len) == 0)
            return &polling_table[i];
    }
    return NULL;
}
