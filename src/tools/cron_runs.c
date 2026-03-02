#include "seaclaw/tool.h"
#include <stdint.h>
#include "seaclaw/tools/cron_runs.h"
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define CRON_RUNS_PARAMS "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"job_id\"]}"

typedef struct { sc_cron_scheduler_t *sched; } sc_cron_tool_ctx_t;

static sc_error_t cron_runs_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_cron_tool_ctx_t *tctx = (sc_cron_tool_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *job_id_str = sc_json_get_string(args, "job_id");
    if (!job_id_str || job_id_str[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'job_id' parameter", 26);
        return SC_OK;
    }
    char *end = NULL;
    unsigned long long id_val = strtoull(job_id_str, &end, 10);
    if (end == job_id_str || *end != '\0' || id_val == 0) {
        *out = sc_tool_result_fail("invalid job_id", 15);
        return SC_OK;
    }
    uint64_t job_id = (uint64_t)id_val;
    (void)job_id;
    double limit_val = sc_json_get_number(args, "limit", 10);
    size_t limit = (size_t)(limit_val > 0 ? limit_val : 10);
    if (limit > 100) limit = 100;

#if SC_IS_TEST
    sc_cron_scheduler_t *sched = sc_cron_create(alloc, 100, true);
    if (!sched) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    sc_cron_add_job(sched, alloc, "* * * * *", "echo x", NULL, &added_id);
    int64_t now = (int64_t)time(NULL);
    sc_cron_add_run(sched, alloc, job_id, now, "executed", "output");
    if (added_id == job_id)
        sc_cron_add_run(sched, alloc, job_id, now - 1, "executed", NULL);

    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(sched, job_id, limit, &count);

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) goto fail;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sc_json_buf_append_raw(&buf, ",", 1);
        const sc_cron_run_t *r = &runs[i];
        if (sc_json_buf_append_raw(&buf, "{\"id\":", 6) != SC_OK) goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->id);
        sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        sc_json_buf_append_raw(&buf, ",\"job_id\":", 10);
        nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->job_id);
        sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (r->status) {
            sc_json_buf_append_raw(&buf, ",\"status\":", 10);
            sc_json_append_string(&buf, r->status, strlen(r->status));
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
    if (!tctx || !tctx->sched) {
        *out = sc_tool_result_fail("cron_runs: scheduler not configured", 36);
        return SC_OK;
    }
    sc_cron_scheduler_t *sched = tctx->sched;
    size_t count = 0;
    const sc_cron_run_t *runs = sc_cron_list_runs(sched, job_id, limit, &count);

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) goto fail;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sc_json_buf_append_raw(&buf, ",", 1);
        const sc_cron_run_t *r = &runs[i];
        if (sc_json_buf_append_raw(&buf, "{\"id\":", 6) != SC_OK) goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->id);
        sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        sc_json_buf_append_raw(&buf, ",\"job_id\":", 10);
        nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->job_id);
        sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (r->status) {
            sc_json_buf_append_raw(&buf, ",\"status\":", 10);
            sc_json_append_string(&buf, r->status, strlen(r->status));
        }
        sc_json_buf_append_raw(&buf, "}", 1);
    }
    sc_json_buf_append_raw(&buf, "]", 1);

    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) { fail: sc_json_buf_free(&buf); *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    size_t out_len = buf.len;
    memcpy(msg, buf.ptr, out_len);
    msg[out_len] = '\0';
    sc_json_buf_free(&buf);
    *out = sc_tool_result_ok_owned(msg, out_len);
    return SC_OK;
#endif
}

static const char *cron_runs_name(void *ctx) { (void)ctx; return "cron_runs"; }
static const char *cron_runs_desc(void *ctx) {
    (void)ctx;
    return "List recent execution history for a cron job.";
}
static const char *cron_runs_params(void *ctx) { (void)ctx; return CRON_RUNS_PARAMS; }
static void cron_runs_deinit(void *ctx, sc_allocator_t *alloc) { (void)ctx; (void)alloc; free(ctx); }

static const sc_tool_vtable_t cron_runs_vtable = {
    .execute = cron_runs_execute, .name = cron_runs_name,
    .description = cron_runs_desc, .parameters_json = cron_runs_params,
    .deinit = cron_runs_deinit,
};

sc_error_t sc_cron_runs_create(sc_allocator_t *alloc, sc_cron_scheduler_t *sched, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    (void)sched;
    void *ctx = calloc(1, 1);
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &cron_runs_vtable;
    return SC_OK;
}
