#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/cron.h"
#include "seaclaw/tool.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_CRON_ADD_PARAMS                                                                    \
    "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\"},\"command\":{" \
    "\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"command\"]}"

typedef struct {
    sc_cron_scheduler_t *sched;
} sc_cron_tool_ctx_t;

static sc_error_t cron_add_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                   sc_tool_result_t *out) {
    sc_cron_tool_ctx_t *tctx = (sc_cron_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *command = sc_json_get_string(args, "command");
    if (!command || !command[0]) {
        *out = sc_tool_result_fail("missing required parameter: command", 36);
        return SC_OK;
    }
    const char *expression = sc_json_get_string(args, "expression");
    const char *name = sc_json_get_string(args, "name");

#if SC_IS_TEST
    sc_cron_scheduler_t *sched = sc_cron_create(alloc, 100, true);
    if (!sched) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(sched, alloc, expression, command, name, &id);
    if (err != SC_OK) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("failed to add job", 18);
        return err;
    }
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "{\"id\":", 6) != SC_OK)
        goto fail;
    char nbuf[32];
    int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)id);
    if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK)
        goto fail;
    if (sc_json_buf_append_raw(&buf, ",\"expression\":", 14) != SC_OK)
        goto fail;
    const sc_cron_job_t *job = sc_cron_get_job(sched, id);
    if (job && job->expression)
        sc_json_append_string(&buf, job->expression, strlen(job->expression));
    else
        sc_json_buf_append_raw(&buf, "\"* * * * *\"", 11);
    if (sc_json_buf_append_raw(&buf, ",\"command\":", 11) != SC_OK)
        goto fail;
    sc_json_append_string(&buf, command, strlen(command));
    if (sc_json_buf_append_raw(&buf, "}", 1) != SC_OK)
        goto fail;

    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) {
    fail:
        sc_json_buf_free(&buf);
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = buf.len;
    memcpy(msg, buf.ptr, out_len);
    msg[out_len] = '\0';
    sc_json_buf_free(&buf);
    sc_cron_destroy(sched, alloc);
    *out = sc_tool_result_ok_owned(msg, out_len);
    return SC_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = sc_tool_result_fail("cron_add: scheduler not configured", 35);
        return SC_OK;
    }
    sc_cron_scheduler_t *sched = tctx->sched;
    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(sched, alloc, expression, command, name, &id);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("failed to add job", 18);
        return err;
    }
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "{\"id\":", 6) != SC_OK)
        goto fail;
    char nbuf[32];
    int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)id);
    if (sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen) != SC_OK)
        goto fail;
    if (sc_json_buf_append_raw(&buf, ",\"expression\":", 14) != SC_OK)
        goto fail;
    const sc_cron_job_t *job = sc_cron_get_job(sched, id);
    if (job && job->expression)
        sc_json_append_string(&buf, job->expression, strlen(job->expression));
    else
        sc_json_buf_append_raw(&buf, "\"* * * * *\"", 11);
    if (sc_json_buf_append_raw(&buf, ",\"command\":", 11) != SC_OK)
        goto fail;
    sc_json_append_string(&buf, command, strlen(command));
    if (sc_json_buf_append_raw(&buf, "}", 1) != SC_OK)
        goto fail;

    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) {
    fail:
        sc_json_buf_free(&buf);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t out_len = buf.len;
    memcpy(msg, buf.ptr, out_len);
    msg[out_len] = '\0';
    sc_json_buf_free(&buf);
    *out = sc_tool_result_ok_owned(msg, out_len);
    return SC_OK;
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
    return SC_CRON_ADD_PARAMS;
}
static void cron_add_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t cron_add_vtable = {
    .execute = cron_add_execute,
    .name = cron_add_name,
    .description = cron_add_desc,
    .parameters_json = cron_add_params,
    .deinit = cron_add_deinit,
};

sc_error_t sc_cron_add_create(sc_allocator_t *alloc, sc_cron_scheduler_t *sched, sc_tool_t *out) {
    (void)alloc;
    sc_cron_tool_ctx_t *ctx = (sc_cron_tool_ctx_t *)calloc(1, sizeof(sc_cron_tool_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_add_vtable;
    return SC_OK;
}
