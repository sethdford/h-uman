#include "seaclaw/service.h"
#include "seaclaw/channel_loop.h"
#include <stdatomic.h>
#include <string.h>

#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

static _Atomic bool sc_service_running = false;
static sc_channel_loop_ctx_t *g_loop_ctx = NULL;
static sc_channel_loop_state_t *g_loop_state = NULL;
#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
static pthread_t g_service_thread;
static pthread_mutex_t s_service_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

void sc_service_configure(sc_channel_loop_ctx_t *ctx, sc_channel_loop_state_t *state) {
#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
    pthread_mutex_lock(&s_service_mutex);
#endif
    g_loop_ctx = ctx;
    g_loop_state = state;
#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
    pthread_mutex_unlock(&s_service_mutex);
#endif
}

#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
static void *service_thread_fn(void *arg) {
    (void)arg;
    for (;;) {
        bool running;
        sc_channel_loop_ctx_t *ctx;
        sc_channel_loop_state_t *state;
        pthread_mutex_lock(&s_service_mutex);
        running = atomic_load(&sc_service_running);
        ctx = g_loop_ctx;
        state = g_loop_state;
        pthread_mutex_unlock(&s_service_mutex);
        if (!running || !ctx || !state || sc_channel_loop_should_stop(state))
            break;
        int processed = 0;
        sc_channel_loop_tick(ctx, state, &processed);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

sc_error_t sc_service_start(void) {
#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    atomic_store(&sc_service_running, true);
    return SC_OK;
#elif defined(SC_GATEWAY_POSIX)
    pthread_mutex_lock(&s_service_mutex);
    if (!g_loop_ctx || !g_loop_state) {
        pthread_mutex_unlock(&s_service_mutex);
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load(&sc_service_running)) {
        pthread_mutex_unlock(&s_service_mutex);
        return SC_OK;
    }
    atomic_store(&sc_service_running, true);
    if (g_loop_state)
        g_loop_state->stop_requested = false;
    pthread_t thr = g_service_thread;
    pthread_mutex_unlock(&s_service_mutex);
    if (pthread_create(&thr, NULL, service_thread_fn, NULL) != 0) {
        atomic_store(&sc_service_running, false);
        return SC_ERR_INTERNAL;
    }
    pthread_mutex_lock(&s_service_mutex);
    g_service_thread = thr;
    pthread_mutex_unlock(&s_service_mutex);
    return SC_OK;
#else
    atomic_store(&sc_service_running, true);
    return SC_OK;
#endif
}

void sc_service_stop(void) {
    atomic_store(&sc_service_running, false);
#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
    pthread_mutex_lock(&s_service_mutex);
    sc_channel_loop_state_t *state = g_loop_state;
    pthread_t thr = g_service_thread;
    pthread_mutex_unlock(&s_service_mutex);
    if (state)
        sc_channel_loop_request_stop(state);
    pthread_join(thr, NULL);
#endif
}

bool sc_service_status(void) {
    return atomic_load(&sc_service_running);
}
