#include "human/agent/dispatcher.h"
#include "human/core/json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_GATEWAY_POSIX) && !defined(HU_IS_TEST)
#include <errno.h>
#include <pthread.h>
#endif

static hu_tool_t *find_tool(hu_tool_t *tools, size_t tools_count, const char *name,
                            size_t name_len) {
    for (size_t i = 0; i < tools_count; i++) {
        const char *n = tools[i].vtable->name(tools[i].ctx);
        if (n && name_len == strlen(n) && memcmp(n, name, name_len) == 0) {
            return &tools[i];
        }
    }
    return NULL;
}

static void execute_one(hu_allocator_t *alloc, hu_tool_t *tools, size_t tools_count,
                        const hu_tool_call_t *call, hu_tool_result_t *result_out) {
    hu_tool_t *tool = find_tool(tools, tools_count, call->name, call->name_len);
    if (!tool) {
        *result_out = hu_tool_result_fail("tool not found", 14);
        return;
    }

    hu_json_value_t *args = NULL;
    if (call->arguments_len > 0) {
        hu_error_t err = hu_json_parse(alloc, call->arguments, call->arguments_len, &args);
        if (err != HU_OK)
            args = NULL;
    }
    *result_out = hu_tool_result_fail("invalid arguments", 16);
    if (args) {
        tool->vtable->execute(tool->ctx, alloc, args, result_out);
        hu_json_free(alloc, args);
    }
}

#if defined(HU_GATEWAY_POSIX) && !defined(HU_IS_TEST)
#include <time.h>
typedef struct dispatch_worker_ctx {
    hu_allocator_t *alloc;
    hu_tool_t *tools;
    size_t tools_count;
    const hu_tool_call_t *call;
    hu_tool_result_t result;
    volatile int done;
} dispatch_worker_ctx_t;

static void *dispatch_worker(void *arg);
static int timed_join(pthread_t thread, uint32_t timeout_secs, volatile int *done_flag);
#endif

/* Sequential dispatch — always used when HU_IS_TEST or max_parallel==1 or non-POSIX */
static hu_error_t dispatch_sequential(hu_allocator_t *alloc, hu_tool_t *tools, size_t tools_count,
                                      const hu_tool_call_t *calls, size_t calls_count,
                                      uint32_t timeout_secs, hu_dispatch_result_t *out) {
    hu_tool_result_t *results =
        (hu_tool_result_t *)alloc->alloc(alloc->ctx, calls_count * sizeof(hu_tool_result_t));
    if (!results)
        return HU_ERR_OUT_OF_MEMORY;

#if defined(HU_GATEWAY_POSIX) && !defined(HU_IS_TEST)
    if (timeout_secs > 0) {
        for (size_t i = 0; i < calls_count; i++) {
            dispatch_worker_ctx_t wctx = {.alloc = alloc,
                                          .tools = tools,
                                          .tools_count = tools_count,
                                          .call = &calls[i],
                                          .done = 0};
            memset(&wctx.result, 0, sizeof(hu_tool_result_t));
            pthread_t tid;
            if (pthread_create(&tid, NULL, dispatch_worker, &wctx) != 0) {
                results[i] = hu_tool_result_fail("thread creation failed", 22);
                continue;
            }
            int timed_out = timed_join(tid, timeout_secs, &wctx.done);
            if (timed_out) {
                hu_tool_result_free(alloc, &wctx.result);
                results[i] = hu_tool_result_fail("tool execution timed out", 24);
            } else {
                results[i] = wctx.result;
            }
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
    return HU_OK;
}

#if defined(HU_GATEWAY_POSIX) && !defined(HU_IS_TEST)

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

static hu_error_t dispatch_parallel(hu_dispatcher_t *d, hu_allocator_t *alloc, hu_tool_t *tools,
                                    size_t tools_count, const hu_tool_call_t *calls,
                                    size_t calls_count, hu_dispatch_result_t *out) {
    if (calls_count == 0) {
        out->results = NULL;
        out->count = 0;
        return HU_OK;
    }

    dispatch_worker_ctx_t *ctxs = (dispatch_worker_ctx_t *)alloc->alloc(
        alloc->ctx, calls_count * sizeof(dispatch_worker_ctx_t));
    if (!ctxs)
        return HU_ERR_OUT_OF_MEMORY;

    pthread_t *threads = (pthread_t *)alloc->alloc(alloc->ctx, calls_count * sizeof(pthread_t));
    if (!threads) {
        alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < calls_count; i++) {
        ctxs[i].alloc = alloc;
        ctxs[i].tools = tools;
        ctxs[i].tools_count = tools_count;
        ctxs[i].call = &calls[i];
        ctxs[i].done = 0;
        memset(&ctxs[i].result, 0, sizeof(hu_tool_result_t));

        int rc = pthread_create(&threads[i], NULL, dispatch_worker, &ctxs[i]);
        if (rc != 0) {
            for (size_t j = 0; j < i; j++)
                pthread_join(threads[j], NULL);
            alloc->free(alloc->ctx, threads, calls_count * sizeof(pthread_t));
            alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    for (size_t i = 0; i < calls_count; i++) {
        int timed_out = timed_join(threads[i], d->timeout_secs, &ctxs[i].done);
        if (timed_out) {
            hu_tool_result_free(alloc, &ctxs[i].result);
            ctxs[i].result = hu_tool_result_fail("tool execution timed out", 24);
        }
    }

    hu_tool_result_t *results =
        (hu_tool_result_t *)alloc->alloc(alloc->ctx, calls_count * sizeof(hu_tool_result_t));
    if (!results) {
        alloc->free(alloc->ctx, threads, calls_count * sizeof(pthread_t));
        alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < calls_count; i++) {
        results[i] = ctxs[i].result;
    }

    alloc->free(alloc->ctx, threads, calls_count * sizeof(pthread_t));
    alloc->free(alloc->ctx, ctxs, calls_count * sizeof(dispatch_worker_ctx_t));

    out->results = results;
    out->count = calls_count;
    return HU_OK;
}
#endif

void hu_dispatcher_default(hu_dispatcher_t *out) {
    if (!out)
        return;
    out->max_parallel = 1;
    out->timeout_secs = 0;
}

hu_error_t hu_dispatcher_create(hu_allocator_t *alloc, uint32_t max_parallel, uint32_t timeout_secs,
                                hu_dispatcher_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_dispatcher_t *d = (hu_dispatcher_t *)alloc->alloc(alloc->ctx, sizeof(hu_dispatcher_t));
    if (!d)
        return HU_ERR_OUT_OF_MEMORY;
    d->max_parallel = max_parallel ? max_parallel : 1;
    d->timeout_secs = timeout_secs;
    *out = d;
    return HU_OK;
}

void hu_dispatcher_destroy(hu_allocator_t *alloc, hu_dispatcher_t *d) {
    if (!alloc || !d)
        return;
    alloc->free(alloc->ctx, d, sizeof(hu_dispatcher_t));
}

hu_error_t hu_dispatcher_dispatch(hu_dispatcher_t *d, hu_allocator_t *alloc, hu_tool_t *tools,
                                  size_t tools_count, const hu_tool_call_t *calls,
                                  size_t calls_count, hu_dispatch_result_t *out) {
    if (!d || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->results = NULL;
    out->count = 0;

    if (calls_count == 0)
        return HU_OK;

#if defined(HU_IS_TEST)
    (void)d;
    return dispatch_sequential(alloc, tools, tools_count, calls, calls_count, 0, out);
#else
#if defined(HU_GATEWAY_POSIX)
    if (d->max_parallel > 1 && calls_count > 1) {
        return dispatch_parallel(d, alloc, tools, tools_count, calls, calls_count, out);
    }
#endif
    return dispatch_sequential(alloc, tools, tools_count, calls, calls_count, d->timeout_secs, out);
#endif
}

void hu_dispatch_result_free(hu_allocator_t *alloc, hu_dispatch_result_t *r) {
    if (!alloc || !r || !r->results)
        return;
    for (size_t i = 0; i < r->count; i++) {
        hu_tool_result_free(alloc, &r->results[i]);
    }
    alloc->free(alloc->ctx, r->results, r->count * sizeof(hu_tool_result_t));
    r->results = NULL;
    r->count = 0;
}
