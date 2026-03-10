#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/tool.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "human/tools/schema_common.h"
#define HU_CRON_LIST_NAME   "cron_list"
#define HU_CRON_LIST_DESC   "List cron jobs"
#define HU_CRON_LIST_PARAMS HU_SCHEMA_EMPTY

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_cron_tool_ctx_t;

static hu_error_t cron_list_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    hu_cron_tool_ctx_t *tctx = (hu_cron_tool_ctx_t *)ctx;
    (void)tctx;
    (void)args;
    if (!out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
#if HU_IS_TEST
    hu_cron_scheduler_t *sched = hu_cron_create(alloc, 100, true);
    if (!sched) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    uint64_t id = 0;
    hu_cron_add_job(sched, alloc, "*/5 * * * *", "echo hello", "test-job", &id);

    size_t count = 0;
    const hu_cron_job_t *jobs = hu_cron_list_jobs(sched, &count);

    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (hu_json_buf_append_raw(&buf, "[", 1) != HU_OK)
        goto fail;
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            hu_json_buf_append_raw(&buf, ",", 1);
        const hu_cron_job_t *j = &jobs[i];
        if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
            goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)j->id);
        hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (j->expression) {
            hu_json_buf_append_raw(&buf, ",\"expression\":", 14);
            hu_json_append_string(&buf, j->expression, strlen(j->expression));
        }
        if (j->command) {
            hu_json_buf_append_raw(&buf, ",\"command\":", 11);
            hu_json_append_string(&buf, j->command, strlen(j->command));
        }
        if (j->name) {
            hu_json_buf_append_raw(&buf, ",\"name\":", 8);
            hu_json_append_string(&buf, j->name, strlen(j->name));
        }
        hu_json_buf_append_raw(&buf, "}", 1);
    }
    hu_json_buf_append_raw(&buf, "]", 1);

    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) {
    fail:
        hu_json_buf_free(&buf);
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = buf.len;
    memcpy(msg, buf.ptr, out_len);
    msg[out_len] = '\0';
    hu_json_buf_free(&buf);
    hu_cron_destroy(sched, alloc);
    *out = hu_tool_result_ok_owned(msg, out_len);
    return HU_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = hu_tool_result_fail("cron_list: scheduler not configured", 36);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;
    size_t count = 0;
    const hu_cron_job_t *jobs = hu_cron_list_jobs(sched, &count);

    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (hu_json_buf_append_raw(&buf, "[", 1) != HU_OK)
        goto fail;
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            hu_json_buf_append_raw(&buf, ",", 1);
        const hu_cron_job_t *j = &jobs[i];
        if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
            goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)j->id);
        hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (j->expression) {
            hu_json_buf_append_raw(&buf, ",\"expression\":", 14);
            hu_json_append_string(&buf, j->expression, strlen(j->expression));
        }
        if (j->command) {
            hu_json_buf_append_raw(&buf, ",\"command\":", 11);
            hu_json_append_string(&buf, j->command, strlen(j->command));
        }
        if (j->name) {
            hu_json_buf_append_raw(&buf, ",\"name\":", 8);
            hu_json_append_string(&buf, j->name, strlen(j->name));
        }
        hu_json_buf_append_raw(&buf, "}", 1);
    }
    hu_json_buf_append_raw(&buf, "]", 1);

    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) {
    fail:
        hu_json_buf_free(&buf);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = buf.len;
    memcpy(msg, buf.ptr, out_len);
    msg[out_len] = '\0';
    hu_json_buf_free(&buf);
    *out = hu_tool_result_ok_owned(msg, out_len);
    return HU_OK;
#endif
}

static const char *cron_list_name(void *ctx) {
    (void)ctx;
    return HU_CRON_LIST_NAME;
}
static const char *cron_list_description(void *ctx) {
    (void)ctx;
    return HU_CRON_LIST_DESC;
}
static const char *cron_list_parameters_json(void *ctx) {
    (void)ctx;
    return HU_CRON_LIST_PARAMS;
}
static void cron_list_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_cron_tool_ctx_t));
}

static const hu_tool_vtable_t cron_list_vtable = {
    .execute = cron_list_execute,
    .name = cron_list_name,
    .description = cron_list_description,
    .parameters_json = cron_list_parameters_json,
    .deinit = cron_list_deinit,
};

hu_error_t hu_cron_list_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched, hu_tool_t *out) {
    hu_cron_tool_ctx_t *ctx =
        (hu_cron_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_cron_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_cron_tool_ctx_t));
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_list_vtable;
    return HU_OK;
}
