#include "seaclaw/tool.h"
#include <stdint.h>
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_CRON_LIST_NAME "cron_list"
#define SC_CRON_LIST_DESC "List cron jobs"
#define SC_CRON_LIST_PARAMS "{\"type\":\"object\",\"properties\":{},\"required\":[]}"

static sc_error_t cron_list_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    (void)ctx;
    (void)args;
    if (!out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
#if SC_IS_TEST
    sc_cron_scheduler_t *sched = sc_cron_create(alloc, 100, true);
    if (!sched) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    uint64_t id = 0;
    sc_cron_add_job(sched, alloc, "*/5 * * * *", "echo hello", "test-job", &id);

    size_t count = 0;
    const sc_cron_job_t *jobs = sc_cron_list_jobs(sched, &count);

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) goto fail;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sc_json_buf_append_raw(&buf, ",", 1);
        const sc_cron_job_t *j = &jobs[i];
        if (sc_json_buf_append_raw(&buf, "{\"id\":", 6) != SC_OK) goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)j->id);
        sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (j->expression) {
            sc_json_buf_append_raw(&buf, ",\"expression\":", 14);
            sc_json_append_string(&buf, j->expression, strlen(j->expression));
        }
        if (j->command) {
            sc_json_buf_append_raw(&buf, ",\"command\":", 11);
            sc_json_append_string(&buf, j->command, strlen(j->command));
        }
        if (j->name) {
            sc_json_buf_append_raw(&buf, ",\"name\":", 8);
            sc_json_append_string(&buf, j->name, strlen(j->name));
        }
        sc_json_buf_append_raw(&buf, "}", 1);
    }
    sc_json_buf_append_raw(&buf, "]", 1);

    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) { fail: sc_json_buf_free(&buf); sc_cron_destroy(sched, alloc); *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    size_t out_len = buf.len;
    memcpy(msg, buf.ptr, out_len);
    msg[out_len] = '\0';
    sc_json_buf_free(&buf);
    sc_cron_destroy(sched, alloc);
    *out = sc_tool_result_ok_owned(msg, out_len);
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_fail("cron_list: scheduler not configured", 36);
    return SC_OK;
#endif
}

static const char *cron_list_name(void *ctx) { (void)ctx; return SC_CRON_LIST_NAME; }
static const char *cron_list_description(void *ctx) { (void)ctx; return SC_CRON_LIST_DESC; }
static const char *cron_list_parameters_json(void *ctx) { (void)ctx; return SC_CRON_LIST_PARAMS; }
static void cron_list_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t cron_list_vtable = {
    .execute = cron_list_execute, .name = cron_list_name,
    .description = cron_list_description, .parameters_json = cron_list_parameters_json,
    .deinit = cron_list_deinit,
};

sc_error_t sc_cron_list_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    out->ctx = calloc(1, 1);
    out->vtable = &cron_list_vtable;
    return out->ctx ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}
