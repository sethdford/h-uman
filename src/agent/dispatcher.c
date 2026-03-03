#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/core/json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)
#include <errno.h>
#include <pthread.h>
#endif

static sc_tool_t *find_tool(sc_tool_t *tools, size_t tools_count, const char *name,
                            size_t name_len) {
    for (size_t i = 0; i < tools_count; i++) {
        const char *n = tools[i].vtable->name(tools[i].ctx);
        if (n && name_len == strlen(n) && memcmp(n, name, name_len) == 0) {
            return &tools[i];
        }
    }
    return NULL;
}

static void execute_one(sc_allocator_t *alloc, sc_tool_t *tools, size_t tools_count,
                        const sc_tool_call_t *call, sc_tool_result_t *result_out) {
    sc_tool_t *tool = find_tool(tools, tools_count, call->name, call->name_len);
    if (!tool) {
        *result_out = sc_tool_result_fail("tool not found", 14);
        return;
    }

    sc_json_value_t *args = NULL;
    if (call->arguments_len > 0) {
        sc_error_t err = sc_json_parse(alloc, call->arguments, call->arguments_len, &args);
        if (err != SC_OK)
            args = NULL;
    }
    *result_out = sc_tool_result_fail("invalid arguments", 16);
    if (args) {
        tool->vtable->execute(tool->ctx, alloc, args, result_out);
        sc_json_free(alloc, args);
    }
}

#if defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)
#include <time.h>
typedef struct dispatch_worker_ctx {
    sc_allocator_t *alloc;
    sc_tool_t *tools;
    size_t tools_count;
    const sc_tool_call_t *call;
    sc_tool_result_t result;
    volatile int done;
} dispatch_worker_ctx_t;

static void *dispatch_worker(void *arg);
static int timed_join(pthread_t thread, uint32_t timeout_secs, volatile int *done_flag);
#endif

/* Sequential dispatch — always used when SC_IS_TEST or max_parallel==1 or non-POSIX */
static sc_error_t dispatch_sequential(sc_allocator_t *alloc, sc_tool_t *tools, size_t tools_count,
                                      const sc_tool_call_t *calls, size_t calls_count,
                                      uint32_t timeout_secs, sc_dispatch_result_t *out) {
    sc_tool_result_t *results =
        (sc_tool_result_t *)alloc->alloc(alloc->ctx, calls_count * sizeof(sc_tool_result_t));
    if (!results)
        return SC_ERR_OUT_OF_MEMORY;

#if defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)
    if (timeout_secs > 0) {
        for (size_t i = 0; i < calls_count; i++) {
            dispatch_worker_ctx_t wctx = {.alloc = alloc,
                                          .tools = tools,
                                          .tools_count = tools_count,
                                          .call = &calls[i],
                                          .done = 0};
            memset(&wctx.result, 0, sizeof(sc_tool_result_t));
            pthread_t tid;
            if (pthread_create(&tid, NULL, dispatch_worker, &wctx) != 0) {
                results[i] = sc_tool_result_fail("thread creation failed", 22);
                continue;
            }
            int timed_out = timed_join(tid, timeout_secs, &wctx.done);
            if (timed_out)
                results[i] = sc_tool_result_fail("tool execution timed out", 24);
            else
                results[i] = wctx.result;
        }
    } else
#else
    (void)timeout_secs;
#endif
    {
        for (size_t i = 0; i < calls_count; i++) {
            execute_one(alloc, tools, tools_count, &calls[i], &results[i]);
        }
    }

    out->results = results;
    out->count = calls_count;
    return SC_OK;
}

#if defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)

static void *dispatch_worker(void *arg) {
    dispatch_worker_ctx_t *ctx = (dispatch_worker_ctx_t *)arg;
    execute_one(ctx->alloc, ctx->tools, ctx->tools_count, ctx->call, &ctx->result);
    ctx->done = 1;
    return NULL;
}

static int timed_join(pthread_t thread, uint32_t timeout_secs, volatile int *done_flag) {
    if (timeout_secs == 0) {
        pthread_join(thread, NULL);
        return 0;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_secs;

    while (!*done_flag) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            pthread_cancel(thread);
            pthread_join(thread, NULL);
            return -1;
        }
        struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&sleep_ts, NULL);
    }
    pthread_join(thread, NULL);
    return 0;
}

static sc_error_t dispatch_parallel(sc_dispatcher_t *d, sc_allocator_t *alloc, sc_tool_t *tools,
                                    size_t tools_count, const sc_tool_call_t *calls,
                                    size_t calls_count, sc_dispatch_result_t *out) {
    if (calls_count == 0) {
        out->results = NULL;
        out->count = 0;
        return SC_OK;
    }

    dispatch_worker_ctx_t *ctxs = (dispatch_worker_ctx_t *)alloc->alloc(
        alloc->ctx, calls_count * sizeof(dispatch_worker_ctx_t));
    if (!ctxs)
        return SC_ERR_OUT_OF_MEMORY;

    pthread_t *threads = (pthread_t *)alloc->alloc(alloc->ctx, calls_count * sizeof(pthread_t));
    if (!threads) {
        alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < calls_count; i++) {
        ctxs[i].alloc = alloc;
        ctxs[i].tools = tools;
        ctxs[i].tools_count = tools_count;
        ctxs[i].call = &calls[i];
        ctxs[i].done = 0;
        memset(&ctxs[i].result, 0, sizeof(sc_tool_result_t));

        int rc = pthread_create(&threads[i], NULL, dispatch_worker, &ctxs[i]);
        if (rc != 0) {
            for (size_t j = 0; j < i; j++)
                pthread_join(threads[j], NULL);
            alloc->free(alloc->ctx, threads, calls_count * sizeof(pthread_t));
            alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));
            return SC_ERR_OUT_OF_MEMORY;
        }
    }

    for (size_t i = 0; i < calls_count; i++) {
        int timed_out = timed_join(threads[i], d->timeout_secs, &ctxs[i].done);
        if (timed_out) {
            ctxs[i].result = sc_tool_result_fail("tool execution timed out", 24);
        }
    }

    sc_tool_result_t *results =
        (sc_tool_result_t *)alloc->alloc(alloc->ctx, calls_count * sizeof(sc_tool_result_t));
    if (!results) {
        alloc->free(alloc->ctx, threads, calls_count * sizeof(pthread_t));
        alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < calls_count; i++) {
        results[i] = ctxs[i].result;
    }

    alloc->free(alloc->ctx, threads, calls_count * sizeof(pthread_t));
    alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));

    out->results = results;
    out->count = calls_count;
    return SC_OK;
}
#endif

void sc_dispatcher_default(sc_dispatcher_t *out) {
    if (!out)
        return;
    out->max_parallel = 1;
    out->timeout_secs = 0;
}

sc_error_t sc_dispatcher_create(sc_allocator_t *alloc, uint32_t max_parallel, uint32_t timeout_secs,
                                sc_dispatcher_t **out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_dispatcher_t *d = (sc_dispatcher_t *)alloc->alloc(alloc->ctx, sizeof(sc_dispatcher_t));
    if (!d)
        return SC_ERR_OUT_OF_MEMORY;
    d->max_parallel = max_parallel ? max_parallel : 1;
    d->timeout_secs = timeout_secs;
    *out = d;
    return SC_OK;
}

void sc_dispatcher_destroy(sc_allocator_t *alloc, sc_dispatcher_t *d) {
    if (!alloc || !d)
        return;
    alloc->free(alloc->ctx, d, sizeof(sc_dispatcher_t));
}

sc_error_t sc_dispatcher_dispatch(sc_dispatcher_t *d, sc_allocator_t *alloc, sc_tool_t *tools,
                                  size_t tools_count, const sc_tool_call_t *calls,
                                  size_t calls_count, sc_dispatch_result_t *out) {
    if (!d || !alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    out->results = NULL;
    out->count = 0;

    if (calls_count == 0)
        return SC_OK;

#if defined(SC_IS_TEST)
    (void)d;
    return dispatch_sequential(alloc, tools, tools_count, calls, calls_count, 0, out);
#else
#if defined(SC_GATEWAY_POSIX)
    if (d->max_parallel > 1 && calls_count > 1) {
        return dispatch_parallel(d, alloc, tools, tools_count, calls, calls_count, out);
    }
#endif
    return dispatch_sequential(alloc, tools, tools_count, calls, calls_count, d->timeout_secs, out);
#endif
}

void sc_dispatch_result_free(sc_allocator_t *alloc, sc_dispatch_result_t *r) {
    if (!alloc || !r || !r->results)
        return;
    for (size_t i = 0; i < r->count; i++) {
        sc_tool_result_free(alloc, &r->results[i]);
    }
    alloc->free(alloc->ctx, r->results, r->count * sizeof(sc_tool_result_t));
    r->results = NULL;
    r->count = 0;
}
