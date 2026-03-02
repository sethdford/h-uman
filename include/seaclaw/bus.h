#ifndef SC_BUS_H
#define SC_BUS_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Event types
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_bus_event_type {
    SC_BUS_MESSAGE_RECEIVED,
    SC_BUS_MESSAGE_SENT,
    SC_BUS_TOOL_CALL,
    SC_BUS_ERROR,
    SC_BUS_HEALTH_CHANGE,
    SC_BUS_EVENT_COUNT
} sc_bus_event_type_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Event payload — optional data for each event type
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_BUS_CHANNEL_LEN 32
#define SC_BUS_ID_LEN 128
#define SC_BUS_MSG_LEN 256

typedef struct sc_bus_event {
    sc_bus_event_type_t type;
    char channel[SC_BUS_CHANNEL_LEN];
    char id[SC_BUS_ID_LEN];       /* session_id, chat_id, user_id, etc. */
    char message[SC_BUS_MSG_LEN]; /* optional message or error text */
    void *payload;                /* optional extra data; caller manages lifetime */
} sc_bus_event_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Subscriber callback. Return false to unsubscribe.
 * ────────────────────────────────────────────────────────────────────────── */

typedef bool (*sc_bus_subscriber_fn)(sc_bus_event_type_t type,
                                     const sc_bus_event_t *ev,
                                     void *user_ctx);

typedef struct sc_bus_subscriber {
    sc_bus_subscriber_fn fn;
    void *user_ctx;
    sc_bus_event_type_t filter;  /* SC_BUS_EVENT_COUNT = all events */
} sc_bus_subscriber_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Event bus — thread-safe pub/sub
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_BUS_MAX_SUBSCRIBERS 16

typedef struct sc_bus {
    sc_bus_subscriber_t subscribers[SC_BUS_MAX_SUBSCRIBERS];
    size_t count;
} sc_bus_t;

/* Initialize bus. */
void sc_bus_init(sc_bus_t *bus);

/* Subscribe. Returns SC_ERR_ALREADY_EXISTS if full. filter=SC_BUS_EVENT_COUNT for all. */
sc_error_t sc_bus_subscribe(sc_bus_t *bus,
                            sc_bus_subscriber_fn fn,
                            void *user_ctx,
                            sc_bus_event_type_t filter);

/* Unsubscribe by fn+ctx. */
void sc_bus_unsubscribe(sc_bus_t *bus, sc_bus_subscriber_fn fn, void *user_ctx);

/* Publish event to all matching subscribers. Thread-safe. */
void sc_bus_publish(sc_bus_t *bus, const sc_bus_event_t *ev);

/* Convenience: publish with minimal fields. */
void sc_bus_publish_simple(sc_bus_t *bus,
                           sc_bus_event_type_t type,
                           const char *channel,
                           const char *id,
                           const char *message);

#endif /* SC_BUS_H */
