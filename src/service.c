#include "human/service.h"
#include "human/channel_loop.h"
#include <stdatomic.h>
#include <string.h>

#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

static _Atomic bool hu_service_running = false;
static hu_channel_loop_ctx_t *g_loop_ctx = NULL;
static hu_channel_loop_state_t *g_loop_state = NULL;
#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
static pthread_t g_service_thread;
static pthread_mutex_t s_service_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

void hu_service_configure(hu_channel_loop_ctx_t *ctx, hu_channel_loop_state_t *state) {
#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
    pthread_mutex_lock(&s_service_mutex);
#endif
    g_loop_ctx = ctx;
    g_loop_state = state;
#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
    pthread_mutex_unlock(&s_service_mutex);
#endif
}

#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
static void *service_thread_fn(void *arg) {
    (void)arg;
    for (;;) {
        bool running;
        hu_channel_loop_ctx_t *ctx;
        hu_channel_loop_state_t *state;
        pthread_mutex_lock(&s_service_mutex);
        running = atomic_load(&hu_service_running);
        ctx = g_loop_ctx;
        state = g_loop_state;
        pthread_mutex_unlock(&s_service_mutex);
        if (!running || !ctx || !state || hu_channel_loop_should_stop(state))
            break;
        int processed = 0;
        hu_channel_loop_tick(ctx, state, &processed);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

hu_error_t hu_service_start(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST == 1
    atomic_store(&hu_service_running, true);
    return HU_OK;
#elif defined(HU_GATEWAY_POSIX)
    pthread_mutex_lock(&s_service_mutex);
    if (!g_loop_ctx || !g_loop_state) {
        pthread_mutex_unlock(&s_service_mutex);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load(&hu_service_running)) {
        pthread_mutex_unlock(&s_service_mutex);
        return HU_OK;
    }
    atomic_store(&hu_service_running, true);
    if (g_loop_state)
        g_loop_state->stop_requested = false;
    pthread_t thr = g_service_thread;
    pthread_mutex_unlock(&s_service_mutex);
    if (pthread_create(&thr, NULL, service_thread_fn, NULL) != 0) {
        atomic_store(&hu_service_running, false);
        return HU_ERR_INTERNAL;
    }
    pthread_mutex_lock(&s_service_mutex);
    g_service_thread = thr;
    pthread_mutex_unlock(&s_service_mutex);
    return HU_OK;
#else
    atomic_store(&hu_service_running, true);
    return HU_OK;
#endif
}

void hu_service_stop(void) {
    atomic_store(&hu_service_running, false);
#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || HU_IS_TEST == 0)
    pthread_mutex_lock(&s_service_mutex);
    hu_channel_loop_state_t *state = g_loop_state;
    pthread_t thr = g_service_thread;
    pthread_mutex_unlock(&s_service_mutex);
    if (state)
        hu_channel_loop_request_stop(state);
    pthread_join(thr, NULL);
#endif
}

bool hu_service_status(void) {
    return atomic_load(&hu_service_running);
}
