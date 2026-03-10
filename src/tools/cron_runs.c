#include "human/tools/cron_runs.h"
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
#include <time.h>

#define CRON_RUNS_PARAMS                                                                         \
    "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"limit\":{\"type\":" \
    "\"integer\"}},\"required\":[\"job_id\"]}"

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_cron_tool_ctx_t;

static hu_error_t cron_runs_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    hu_cron_tool_ctx_t *tctx = (hu_cron_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *job_id_str = hu_json_get_string(args, "job_id");
    if (!job_id_str || job_id_str[0] == '\0') {
        *out = hu_tool_result_fail("Missing 'job_id' parameter", 26);
        return HU_OK;
    }
    char *end = NULL;
    unsigned long long id_val = strtoull(job_id_str, &end, 10);
    if (end == job_id_str || *end != '\0' || id_val == 0) {
        *out = hu_tool_result_fail("invalid job_id", 15);
        return HU_OK;
    }
    uint64_t job_id = (uint64_t)id_val;
    (void)job_id;
    double limit_val = hu_json_get_number(args, "limit", 10);
    size_t limit = (size_t)(limit_val > 0 ? limit_val : 10);
    if (limit > 100)
        limit = 100;

#if HU_IS_TEST
    hu_cron_scheduler_t *sched = hu_cron_create(alloc, 100, true);
    if (!sched) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    hu_cron_add_job(sched, alloc, "* * * * *", "echo x", NULL, &added_id);
    int64_t now = (int64_t)time(NULL);
    hu_cron_add_run(sched, alloc, job_id, now, "executed", "output");
    if (added_id == job_id)
        hu_cron_add_run(sched, alloc, job_id, now - 1, "executed", NULL);

    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(sched, job_id, limit, &count);

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
        const hu_cron_run_t *r = &runs[i];
        if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
            goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->id);
        hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        hu_json_buf_append_raw(&buf, ",\"job_id\":", 10);
        nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->job_id);
        hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (r->status) {
            hu_json_buf_append_raw(&buf, ",\"status\":", 10);
            hu_json_append_string(&buf, r->status, strlen(r->status));
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
        *out = hu_tool_result_fail("cron_runs: scheduler not configured", 36);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;
    size_t count = 0;
    const hu_cron_run_t *runs = hu_cron_list_runs(sched, job_id, limit, &count);

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
        const hu_cron_run_t *r = &runs[i];
        if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
            goto fail;
        char nbuf[32];
        int nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->id);
        hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        hu_json_buf_append_raw(&buf, ",\"job_id\":", 10);
        nlen = snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)r->job_id);
        hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
        if (r->status) {
            hu_json_buf_append_raw(&buf, ",\"status\":", 10);
            hu_json_append_string(&buf, r->status, strlen(r->status));
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

static const char *cron_runs_name(void *ctx) {
    (void)ctx;
    return "cron_runs";
}
static const char *cron_runs_desc(void *ctx) {
    (void)ctx;
    return "List recent execution history for a cron job.";
}
static const char *cron_runs_params(void *ctx) {
    (void)ctx;
    return CRON_RUNS_PARAMS;
}
static void cron_runs_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_cron_tool_ctx_t));
}

static const hu_tool_vtable_t cron_runs_vtable = {
    .execute = cron_runs_execute,
    .name = cron_runs_name,
    .description = cron_runs_desc,
    .parameters_json = cron_runs_params,
    .deinit = cron_runs_deinit,
};

hu_error_t hu_cron_runs_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_cron_tool_ctx_t *ctx =
        (hu_cron_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_cron_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_cron_tool_ctx_t));
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_runs_vtable;
    return HU_OK;
}
