#include "seaclaw/bus.h"
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
typedef CRITICAL_SECTION sc_mutex_t;
#define sc_mutex_init(m)    InitializeCriticalSection(m)
#define sc_mutex_lock(m)    EnterCriticalSection(m)
#define sc_mutex_unlock(m)  LeaveCriticalSection(m)
#define sc_mutex_destroy(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t sc_mutex_t;
#define sc_mutex_init(m)    pthread_mutex_init(m, NULL)
#define sc_mutex_lock(m)    pthread_mutex_lock(m)
#define sc_mutex_unlock(m)  pthread_mutex_unlock(m)
#define sc_mutex_destroy(m) pthread_mutex_destroy(m)
#endif

#if defined(_WIN32) || defined(_WIN64)
static sc_mutex_t s_bus_mutex;
static volatile LONG s_bus_once = 0;
static void ensure_mutex(void) {
    if (InterlockedCompareExchange(&s_bus_once, 1, 0) == 0)
        sc_mutex_init(&s_bus_mutex);
}
#else
static sc_mutex_t s_bus_mutex;
static pthread_once_t s_bus_once = PTHREAD_ONCE_INIT;
static void init_bus_mutex(void) {
    sc_mutex_init(&s_bus_mutex);
}
static void ensure_mutex(void) {
    pthread_once(&s_bus_once, init_bus_mutex);
}
#endif

static void lock_bus(sc_bus_t *bus) {
    if (bus->mutex_initialized) {
#if defined(_WIN32) || defined(_WIN64)
        EnterCriticalSection(&bus->mutex);
#else
        pthread_mutex_lock(&bus->mutex);
#endif
    } else {
        ensure_mutex();
        sc_mutex_lock(&s_bus_mutex);
    }
}

static void unlock_bus(sc_bus_t *bus) {
    if (bus->mutex_initialized) {
#if defined(_WIN32) || defined(_WIN64)
        LeaveCriticalSection(&bus->mutex);
#else
        pthread_mutex_unlock(&bus->mutex);
#endif
    } else {
        sc_mutex_unlock(&s_bus_mutex);
    }
}

void sc_bus_init(sc_bus_t *bus) {
    if (!bus)
        return;
    memset(bus, 0, sizeof(*bus));
#if defined(_WIN32) || defined(_WIN64)
    InitializeCriticalSection(&bus->mutex);
#else
    pthread_mutex_init(&bus->mutex, NULL);
#endif
    bus->mutex_initialized = true;
}

void sc_bus_deinit(sc_bus_t *bus) {
    if (!bus || !bus->mutex_initialized)
        return;
#if defined(_WIN32) || defined(_WIN64)
    DeleteCriticalSection(&bus->mutex);
#else
    pthread_mutex_destroy(&bus->mutex);
#endif
    bus->mutex_initialized = false;
}

sc_error_t sc_bus_subscribe(sc_bus_t *bus, sc_bus_subscriber_fn fn, void *user_ctx,
                            sc_bus_event_type_t filter) {
    if (!bus || !fn)
        return SC_ERR_INVALID_ARGUMENT;
    lock_bus(bus);
    if (bus->count >= SC_BUS_MAX_SUBSCRIBERS) {
        unlock_bus(bus);
        return SC_ERR_ALREADY_EXISTS;
    }
    sc_bus_subscriber_t *sub = &bus->subscribers[bus->count++];
    sub->fn = fn;
    sub->user_ctx = user_ctx;
    sub->filter = filter;
    unlock_bus(bus);
    return SC_OK;
}

void sc_bus_unsubscribe(sc_bus_t *bus, sc_bus_subscriber_fn fn, void *user_ctx) {
    if (!bus || !fn)
        return;
    lock_bus(bus);
    for (size_t i = 0; i < bus->count; i++) {
        if (bus->subscribers[i].fn == fn && bus->subscribers[i].user_ctx == user_ctx) {
            memmove(&bus->subscribers[i], &bus->subscribers[i + 1],
                    (bus->count - 1 - i) * sizeof(sc_bus_subscriber_t));
            bus->count--;
            break;
        }
    }
    unlock_bus(bus);
}

void sc_bus_publish(sc_bus_t *bus, const sc_bus_event_t *ev) {
    if (!bus || !ev)
        return;
    lock_bus(bus);
    sc_bus_subscriber_t snapshot[SC_BUS_MAX_SUBSCRIBERS];
    size_t n = bus->count;
    if (n > SC_BUS_MAX_SUBSCRIBERS)
        n = SC_BUS_MAX_SUBSCRIBERS;
    memcpy(snapshot, bus->subscribers, n * sizeof(sc_bus_subscriber_t));
    unlock_bus(bus);

    for (size_t i = 0; i < n; i++) {
        sc_bus_subscriber_t *sub = &snapshot[i];
        if (sub->filter != SC_BUS_EVENT_COUNT && sub->filter != ev->type)
            continue;
        if (!sub->fn(ev->type, ev, sub->user_ctx)) {
            sc_bus_unsubscribe(bus, sub->fn, sub->user_ctx);
        }
    }
}

void sc_bus_publish_simple(sc_bus_t *bus, sc_bus_event_type_t type, const char *channel,
                           const char *id, const char *message) {
    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (channel) {
        size_t cl = strlen(channel);
        if (cl >= SC_BUS_CHANNEL_LEN)
            cl = SC_BUS_CHANNEL_LEN - 1;
        memcpy(ev.channel, channel, cl);
        ev.channel[cl] = '\0';
    }
    if (id) {
        size_t il = strlen(id);
        if (il >= SC_BUS_ID_LEN)
            il = SC_BUS_ID_LEN - 1;
        memcpy(ev.id, id, il);
        ev.id[il] = '\0';
    }
    if (message) {
        size_t ml = strlen(message);
        if (ml >= SC_BUS_MSG_LEN)
            ml = SC_BUS_MSG_LEN - 1;
        memcpy(ev.message, message, ml);
        ev.message[ml] = '\0';
    }
    sc_bus_publish(bus, &ev);
}
