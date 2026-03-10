#ifndef HU_CHANNEL_MANAGER_H
#define HU_CHANNEL_MANAGER_H

#include "bus.h"
#include "channel.h"
#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Listener type — how the channel receives messages
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_channel_listener_type {
    HU_CHANNEL_LISTENER_POLLING,   /* poll in a loop (Telegram, Signal, Matrix) */
    HU_CHANNEL_LISTENER_GATEWAY,   /* internal socket/WebSocket loop */
    HU_CHANNEL_LISTENER_WEBHOOK,   /* HTTP gateway receives */
    HU_CHANNEL_LISTENER_SEND_ONLY, /* outbound only */
    HU_CHANNEL_LISTENER_NONE,
} hu_channel_listener_type_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel entry — one configured channel instance
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_channel_entry {
    const char *name;
    const char *account_id;
    hu_channel_t channel;
    hu_channel_listener_type_t listener_type;
} hu_channel_entry_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel manager — lifecycle, registry, config-driven creation
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_CHANNEL_MANAGER_MAX 32

typedef struct hu_channel_manager {
    hu_allocator_t *alloc;
    hu_channel_entry_t entries[HU_CHANNEL_MANAGER_MAX];
    size_t count;
    hu_bus_t *event_bus;
} hu_channel_manager_t;

/* Initialize manager. */
hu_error_t hu_channel_manager_init(hu_channel_manager_t *mgr, hu_allocator_t *alloc);

/* Free resources. Channels must be stopped first. */
void hu_channel_manager_deinit(hu_channel_manager_t *mgr);

/* Set optional event bus for channels that support it. */
void hu_channel_manager_set_bus(hu_channel_manager_t *mgr, hu_bus_t *bus);

/* Register a channel. Returns HU_ERR_ALREADY_EXISTS if full. */
hu_error_t hu_channel_manager_register(hu_channel_manager_t *mgr, const char *name,
                                       const char *account_id, const hu_channel_t *channel,
                                       hu_channel_listener_type_t listener_type);

/* Start all channels (calls channel->vtable->start). */
hu_error_t hu_channel_manager_start_all(hu_channel_manager_t *mgr);

/* Stop all channels (calls channel->vtable->stop). */
void hu_channel_manager_stop_all(hu_channel_manager_t *mgr);

/* Get entries. */
const hu_channel_entry_t *hu_channel_manager_entries(const hu_channel_manager_t *mgr,
                                                     size_t *out_count);

/* Number of registered channels. */
size_t hu_channel_manager_count(const hu_channel_manager_t *mgr);

#endif /* HU_CHANNEL_MANAGER_H */
