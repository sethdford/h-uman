#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/tool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CRON_RUN_PARAMS                                                                           \
    "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"}},\"required\":[\"job_" \
    "id\"]}"

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_cron_tool_ctx_t;

static hu_error_t cron_run_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_cron_tool_ctx_t *tctx = (hu_cron_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *job_id_str = hu_json_get_string(args, "job_id");
    if (!job_id_str || job_id_str[0] == '\0') {
        *out = hu_tool_result_fail("missing job_id", 14);
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

#if HU_IS_TEST
    hu_cron_scheduler_t *sched = hu_cron_create(alloc, 100, true);
    if (!sched) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    hu_cron_add_job(sched, alloc, "* * * * *", "echo hello", NULL, &added_id);
    const hu_cron_job_t *job = hu_cron_get_job(sched, job_id);
    if (!job) {
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("job not found", 14);
        return HU_OK;
    }
    int64_t now = (int64_t)time(NULL);
    hu_error_t err = hu_cron_add_run(sched, alloc, job_id, now, "executed", NULL);
    hu_cron_destroy(sched, alloc);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("failed to record run", 19);
        return err;
    }
    char *msg = hu_sprintf(alloc, "{\"job_id\":\"%llu\",\"status\":\"executed\"}",
                           (unsigned long long)job_id);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = hu_tool_result_fail("cron_run: scheduler not configured", 35);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;
    const hu_cron_job_t *job = hu_cron_get_job(sched, job_id);
    if (!job) {
        *out = hu_tool_result_fail("job not found", 14);
        return HU_OK;
    }
    int64_t now = (int64_t)time(NULL);
    hu_error_t err = hu_cron_add_run(sched, alloc, job_id, now, "executed", NULL);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("failed to record run", 19);
        return err;
    }
    char *msg = hu_sprintf(alloc, "{\"job_id\":\"%llu\",\"status\":\"executed\"}",
                           (unsigned long long)job_id);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#endif
}

static const char *cron_run_name(void *ctx) {
    (void)ctx;
    return "cron_run";
}
static const char *cron_run_desc(void *ctx) {
    (void)ctx;
    return "Run cron job";
}
static const char *cron_run_params(void *ctx) {
    (void)ctx;
    return CRON_RUN_PARAMS;
}
static void cron_run_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_cron_tool_ctx_t));
}

static const hu_tool_vtable_t cron_run_vtable = {
    .execute = cron_run_execute,
    .name = cron_run_name,
    .description = cron_run_desc,
    .parameters_json = cron_run_params,
    .deinit = cron_run_deinit,
};

hu_error_t hu_cron_run_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched, hu_tool_t *out) {
    hu_cron_tool_ctx_t *ctx =
        (hu_cron_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_cron_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_cron_tool_ctx_t));
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_run_vtable;
    return HU_OK;
}
