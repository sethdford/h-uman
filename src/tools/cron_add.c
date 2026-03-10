#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/tool.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_CRON_ADD_PARAMS                                                                    \
    "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\"},\"command\":{" \
    "\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"command\"]}"

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_cron_tool_ctx_t;

static hu_error_t cron_add_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_cron_tool_ctx_t *tctx = (hu_cron_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *command = hu_json_get_string(args, "command");
    if (!command || !command[0]) {
        *out = hu_tool_result_fail("missing required parameter: command", 36);
        return HU_OK;
    }
    const char *expression = hu_json_get_string(args, "expression");
    const char *name = hu_json_get_string(args, "name");

#if HU_IS_TEST
    hu_cron_scheduler_t *sched = hu_cron_create(alloc, 100, true);
    if (!sched) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(sched, alloc, expression, command, name, &id);
    if (err != HU_OK) {
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("failed to add job", 18);
        return err;
    }
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
        goto fail;
    char nbuf[32];
    int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)id);
    if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
        goto fail;
    if (hu_json_buf_append_raw(&buf, ",\"expression\":", 14) != HU_OK)
        goto fail;
    const hu_cron_job_t *job = hu_cron_get_job(sched, id);
    if (job && job->expression)
        hu_json_append_string(&buf, job->expression, strlen(job->expression));
    else
        hu_json_buf_append_raw(&buf, "\"* * * * *\"", 11);
    if (hu_json_buf_append_raw(&buf, ",\"command\":", 11) != HU_OK)
        goto fail;
    hu_json_append_string(&buf, command, strlen(command));
    if (hu_json_buf_append_raw(&buf, "}", 1) != HU_OK)
        goto fail;

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
        *out = hu_tool_result_fail("cron_add: scheduler not configured", 35);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;
    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(sched, alloc, expression, command, name, &id);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("failed to add job", 18);
        return err;
    }
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
        goto fail;
    char nbuf[32];
    int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)id);
    if (hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != HU_OK)
        goto fail;
    if (hu_json_buf_append_raw(&buf, ",\"expression\":", 14) != HU_OK)
        goto fail;
    const hu_cron_job_t *job = hu_cron_get_job(sched, id);
    if (job && job->expression)
        hu_json_append_string(&buf, job->expression, strlen(job->expression));
    else
        hu_json_buf_append_raw(&buf, "\"* * * * *\"", 11);
    if (hu_json_buf_append_raw(&buf, ",\"command\":", 11) != HU_OK)
        goto fail;
    hu_json_append_string(&buf, command, strlen(command));
    if (hu_json_buf_append_raw(&buf, "}", 1) != HU_OK)
        goto fail;

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

static const char *cron_add_name(void *ctx) {
    (void)ctx;
    return "cron_add";
}
static const char *cron_add_desc(void *ctx) {
    (void)ctx;
    return "Add cron job";
}
static const char *cron_add_params(void *ctx) {
    (void)ctx;
    return HU_CRON_ADD_PARAMS;
}
static void cron_add_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const hu_tool_vtable_t cron_add_vtable = {
    .execute = cron_add_execute,
    .name = cron_add_name,
    .description = cron_add_desc,
    .parameters_json = cron_add_params,
    .deinit = cron_add_deinit,
};

hu_error_t hu_cron_add_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched, hu_tool_t *out) {
    hu_cron_tool_ctx_t *ctx =
        (hu_cron_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_cron_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_cron_tool_ctx_t));
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_add_vtable;
    return HU_OK;
}
