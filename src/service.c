#include "seaclaw/service.h"
#include "seaclaw/channel_loop.h"
#include <string.h>

#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

static volatile bool sc_service_running = false;
static sc_channel_loop_ctx_t *g_loop_ctx = NULL;
static sc_channel_loop_state_t *g_loop_state = NULL;
#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
static pthread_t g_service_thread;
#endif

void sc_service_configure(sc_channel_loop_ctx_t *ctx, sc_channel_loop_state_t *state) {
    g_loop_ctx = ctx;
    g_loop_state = state;
}

#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
static void *service_thread_fn(void *arg) {
    (void)arg;
    while (sc_service_running && g_loop_ctx && g_loop_state &&
           !sc_channel_loop_should_stop(g_loop_state)) {
        int processed = 0;
        sc_channel_loop_tick(g_loop_ctx, g_loop_state, &processed);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

sc_error_t sc_service_start(void) {
#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    sc_service_running = true;
    return SC_OK;
#elif defined(SC_GATEWAY_POSIX)
    if (!g_loop_ctx || !g_loop_state)
        return SC_ERR_INVALID_ARGUMENT;
    if (sc_service_running)
        return SC_OK;
    sc_service_running = true;
    if (g_loop_state)
        g_loop_state->stop_requested = false;
    if (pthread_create(&g_service_thread, NULL, service_thread_fn, NULL) != 0) {
        sc_service_running = false;
        return SC_ERR_INTERNAL;
    }
    return SC_OK;
#else
    sc_service_running = true;
    return SC_OK;
#endif
}

void sc_service_stop(void) {
    sc_service_running = false;
#if defined(SC_GATEWAY_POSIX) && (!defined(SC_IS_TEST) || SC_IS_TEST == 0)
    if (g_loop_state)
        sc_channel_loop_request_stop(g_loop_state);
    pthread_join(g_service_thread, NULL);
#endif
}

bool sc_service_status(void) {
    return sc_service_running;
}
