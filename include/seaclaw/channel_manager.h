#ifndef SC_CHANNEL_MANAGER_H
#define SC_CHANNEL_MANAGER_H

#include "core/allocator.h"
#include "core/error.h"
#include "channel.h"
#include "bus.h"
#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Listener type — how the channel receives messages
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_channel_listener_type {
    SC_CHANNEL_LISTENER_POLLING,     /* poll in a loop (Telegram, Signal, Matrix) */
    SC_CHANNEL_LISTENER_GATEWAY,     /* internal socket/WebSocket loop */
    SC_CHANNEL_LISTENER_WEBHOOK,     /* HTTP gateway receives */
    SC_CHANNEL_LISTENER_SEND_ONLY,   /* outbound only */
    SC_CHANNEL_LISTENER_NONE,
} sc_channel_listener_type_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel entry — one configured channel instance
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_channel_entry {
    const char *name;
    const char *account_id;
    sc_channel_t channel;
    sc_channel_listener_type_t listener_type;
} sc_channel_entry_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Channel manager — lifecycle, registry, config-driven creation
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_CHANNEL_MANAGER_MAX 32

typedef struct sc_channel_manager {
    sc_allocator_t *alloc;
    sc_channel_entry_t entries[SC_CHANNEL_MANAGER_MAX];
    size_t count;
    sc_bus_t *event_bus;
} sc_channel_manager_t;

/* Initialize manager. */
sc_error_t sc_channel_manager_init(sc_channel_manager_t *mgr,
                                   sc_allocator_t *alloc);

/* Free resources. Channels must be stopped first. */
void sc_channel_manager_deinit(sc_channel_manager_t *mgr);

/* Set optional event bus for channels that support it. */
void sc_channel_manager_set_bus(sc_channel_manager_t *mgr, sc_bus_t *bus);

/* Register a channel. Returns SC_ERR_ALREADY_EXISTS if full. */
sc_error_t sc_channel_manager_register(sc_channel_manager_t *mgr,
                                       const char *name,
                                       const char *account_id,
                                       const sc_channel_t *channel,
                                       sc_channel_listener_type_t listener_type);

/* Start all channels (calls channel->vtable->start). */
sc_error_t sc_channel_manager_start_all(sc_channel_manager_t *mgr);

/* Stop all channels (calls channel->vtable->stop). */
void sc_channel_manager_stop_all(sc_channel_manager_t *mgr);

/* Get entries. */
const sc_channel_entry_t *sc_channel_manager_entries(const sc_channel_manager_t *mgr,
                                                      size_t *out_count);

/* Number of registered channels. */
size_t sc_channel_manager_count(const sc_channel_manager_t *mgr);

#endif /* SC_CHANNEL_MANAGER_H */
