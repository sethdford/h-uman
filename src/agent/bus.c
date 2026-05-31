#include "human/bus.h"
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
typedef CRITICAL_SECTION hu_mutex_t;
#define hu_mutex_init(m)    InitializeCriticalSection(m)
#define hu_mutex_lock(m)    EnterCriticalSection(m)
#define hu_mutex_unlock(m)  LeaveCriticalSection(m)
#define hu_mutex_destroy(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t hu_mutex_t;
#define hu_mutex_init(m)    pthread_mutex_init(m, NULL)
#define hu_mutex_lock(m)    pthread_mutex_lock(m)
#define hu_mutex_unlock(m)  pthread_mutex_unlock(m)
#define hu_mutex_destroy(m) pthread_mutex_destroy(m)
#endif

#if defined(_WIN32) || defined(_WIN64)
static hu_mutex_t s_bus_mutex;
static volatile LONG s_bus_once = 0;
static void ensure_mutex(void) {
    if (InterlockedCompareExchange(&s_bus_once, 1, 0) == 0)
        hu_mutex_init(&s_bus_mutex);
}
#else
static hu_mutex_t s_bus_mutex;
static pthread_once_t s_bus_once = PTHREAD_ONCE_INIT;
static void init_bus_mutex(void) {
    hu_mutex_init(&s_bus_mutex);
}
static void ensure_mutex(void) {
    pthread_once(&s_bus_once, init_bus_mutex);
}
#endif

static void lock_bus(hu_bus_t *bus) {
    if (bus->mutex_initialized) {
#if defined(_WIN32) || defined(_WIN64)
        EnterCriticalSection(&bus->mutex);
#else
        pthread_mutex_lock(&bus->mutex);
#endif
    } else {
        ensure_mutex();
        hu_mutex_lock(&s_bus_mutex);
    }
}

static void unlock_bus(hu_bus_t *bus) {
    if (bus->mutex_initialized) {
#if defined(_WIN32) || defined(_WIN64)
        LeaveCriticalSection(&bus->mutex);
#else
        pthread_mutex_unlock(&bus->mutex);
#endif
    } else {
        hu_mutex_unlock(&s_bus_mutex);
    }
}

void hu_bus_init(hu_bus_t *bus) {
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

void hu_bus_deinit(hu_bus_t *bus) {
    if (!bus || !bus->mutex_initialized)
        return;
#if defined(_WIN32) || defined(_WIN64)
    DeleteCriticalSection(&bus->mutex);
#else
    pthread_mutex_destroy(&bus->mutex);
#endif
    bus->mutex_initialized = false;
}

hu_error_t hu_bus_subscribe(hu_bus_t *bus, hu_bus_subscriber_fn fn, void *user_ctx,
                            hu_bus_event_type_t filter) {
    if (!bus || !fn)
        return HU_ERR_INVALID_ARGUMENT;
    lock_bus(bus);
    if (bus->count >= HU_BUS_MAX_SUBSCRIBERS) {
        unlock_bus(bus);
        return HU_ERR_ALREADY_EXISTS;
    }
    hu_bus_subscriber_t *sub = &bus->subscribers[bus->count++];
    sub->fn = fn;
    sub->user_ctx = user_ctx;
    sub->filter = filter;
    unlock_bus(bus);
    return HU_OK;
}

void hu_bus_unsubscribe(hu_bus_t *bus, hu_bus_subscriber_fn fn, void *user_ctx) {
    if (!bus || !fn)
        return;
    lock_bus(bus);
    for (size_t i = 0; i < bus->count; i++) {
        if (bus->subscribers[i].fn == fn && bus->subscribers[i].user_ctx == user_ctx) {
            memmove(&bus->subscribers[i], &bus->subscribers[i + 1],
                    (bus->count - 1 - i) * sizeof(hu_bus_subscriber_t));
            bus->count--;
            break;
        }
    }
    unlock_bus(bus);
}

void hu_bus_publish(hu_bus_t *bus, const hu_bus_event_t *ev) {
    if (!bus || !ev)
        return;
    lock_bus(bus);
    hu_bus_subscriber_t snapshot[HU_BUS_MAX_SUBSCRIBERS];
    size_t n = bus->count;
    if (n > HU_BUS_MAX_SUBSCRIBERS)
        n = HU_BUS_MAX_SUBSCRIBERS;
    memcpy(snapshot, bus->subscribers, n * sizeof(hu_bus_subscriber_t));
    unlock_bus(bus);

    for (size_t i = 0; i < n; i++) {
        hu_bus_subscriber_t *sub = &snapshot[i];
        if (sub->filter != HU_BUS_EVENT_COUNT && sub->filter != ev->type)
            continue;
        if (!sub->fn(ev->type, ev, sub->user_ctx)) {
            hu_bus_unsubscribe(bus, sub->fn, sub->user_ctx);
        }
    }
}

void hu_bus_publish_simple(hu_bus_t *bus, hu_bus_event_type_t type, const char *channel,
                           const char *id, const char *message) {
    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (channel) {
        size_t cl = strlen(channel);
        if (cl >= HU_BUS_CHANNEL_LEN)
            cl = HU_BUS_CHANNEL_LEN - 1;
        memcpy(ev.channel, channel, cl);
        ev.channel[cl] = '\0';
    }
    if (id) {
        size_t il = strlen(id);
        if (il >= HU_BUS_ID_LEN)
            il = HU_BUS_ID_LEN - 1;
        memcpy(ev.id, id, il);
        ev.id[il] = '\0';
    }
    if (message) {
        size_t ml = strlen(message);
        if (ml >= HU_BUS_MSG_LEN)
            ml = HU_BUS_MSG_LEN - 1;
        memcpy(ev.message, message, ml);
        ev.message[ml] = '\0';
    }
    hu_bus_publish(bus, &ev);
}
