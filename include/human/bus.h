#ifndef HU_BUS_H
#define HU_BUS_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Event types
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_bus_event_type {
    HU_BUS_MESSAGE_RECEIVED,
    HU_BUS_MESSAGE_SENT,
    HU_BUS_MESSAGE_CHUNK,
    HU_BUS_TOOL_CALL,
    HU_BUS_ERROR,
    HU_BUS_HEALTH_CHANGE,
    HU_BUS_CRON_STARTED,
    HU_BUS_CRON_COMPLETED,
    HU_BUS_EVENT_COUNT
} hu_bus_event_type_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Event payload — optional data for each event type
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_BUS_CHANNEL_LEN 32
#define HU_BUS_ID_LEN      128
#define HU_BUS_MSG_LEN     256

typedef struct hu_bus_event {
    hu_bus_event_type_t type;
    char channel[HU_BUS_CHANNEL_LEN];
    char id[HU_BUS_ID_LEN];       /* session_id, chat_id, user_id, etc. */
    char message[HU_BUS_MSG_LEN]; /* optional message or error text */
    void *payload;                /* optional extra data; caller manages lifetime */
} hu_bus_event_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Subscriber callback. Return false to unsubscribe.
 * ────────────────────────────────────────────────────────────────────────── */

typedef bool (*hu_bus_subscriber_fn)(hu_bus_event_type_t type, const hu_bus_event_t *ev,
                                     void *user_ctx);

typedef struct hu_bus_subscriber {
    hu_bus_subscriber_fn fn;
    void *user_ctx;
    hu_bus_event_type_t filter; /* HU_BUS_EVENT_COUNT = all events */
} hu_bus_subscriber_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Event bus — thread-safe pub/sub
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_BUS_MAX_SUBSCRIBERS 16

typedef struct hu_bus {
    hu_bus_subscriber_t subscribers[HU_BUS_MAX_SUBSCRIBERS];
    size_t count;
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION mutex;
#else
    pthread_mutex_t mutex;
#endif
    bool mutex_initialized;
} hu_bus_t;

/* Initialize bus. */
void hu_bus_init(hu_bus_t *bus);

/* Destroy bus (call when done). */
void hu_bus_deinit(hu_bus_t *bus);

/* Subscribe. Returns HU_ERR_ALREADY_EXISTS if full. filter=HU_BUS_EVENT_COUNT for all. */
hu_error_t hu_bus_subscribe(hu_bus_t *bus, hu_bus_subscriber_fn fn, void *user_ctx,
                            hu_bus_event_type_t filter);

/* Unsubscribe by fn+ctx. */
void hu_bus_unsubscribe(hu_bus_t *bus, hu_bus_subscriber_fn fn, void *user_ctx);

/* Publish event to all matching subscribers. Thread-safe. */
void hu_bus_publish(hu_bus_t *bus, const hu_bus_event_t *ev);

/* Convenience: publish with minimal fields. */
void hu_bus_publish_simple(hu_bus_t *bus, hu_bus_event_type_t type, const char *channel,
                           const char *id, const char *message);

#endif /* HU_BUS_H */
