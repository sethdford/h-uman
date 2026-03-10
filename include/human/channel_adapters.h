#ifndef HU_CHANNEL_ADAPTERS_H
#define HU_CHANNEL_ADAPTERS_H

#include <stddef.h>

/* Channel adapters: polling descriptors, inbound route descriptors.
 * Human: minimal stub — parsePeerKind, findPollingDescriptor. */

/* Polling descriptor: maps channel name to polling interval for HU_LISTENER_POLLING channels. */
typedef struct hu_polling_descriptor {
    const char *channel_name;
    unsigned int interval_ms;
} hu_polling_descriptor_t;

typedef enum hu_chat_type {
    HU_CHAT_DIRECT,
    HU_CHAT_GROUP,
    HU_CHAT_CHANNEL,
} hu_chat_type_t;

/* Parse peer kind string to ChatType. Returns -1 if invalid. */
int hu_channel_adapters_parse_peer_kind(const char *raw, size_t len);

/* Find polling descriptor by channel name. Returns NULL if not found. */
const hu_polling_descriptor_t *hu_channel_adapters_find_polling_descriptor(const char *channel_name,
                                                                           size_t len);

#endif /* HU_CHANNEL_ADAPTERS_H */
