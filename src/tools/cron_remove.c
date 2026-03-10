#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/tool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "human/tools/schema_common.h"
#define HU_CRON_REMOVE_NAME   "cron_remove"
#define HU_CRON_REMOVE_DESC   "Remove cron job"
#define HU_CRON_REMOVE_PARAMS HU_SCHEMA_ID_ONLY

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_cron_tool_ctx_t;

static hu_error_t cron_remove_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    hu_cron_tool_ctx_t *tctx = (hu_cron_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *id_str = hu_json_get_string(args, "id");
    if (!id_str || id_str[0] == '\0') {
        *out = hu_tool_result_fail("missing id", 10);
        return HU_OK;
    }
    char *end = NULL;
    unsigned long long id_val = strtoull(id_str, &end, 10);
    if (end == id_str || *end != '\0' || id_val == 0) {
        *out = hu_tool_result_fail("invalid id", 10);
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
    hu_cron_add_job(sched, alloc, "* * * * *", "echo x", NULL, &added_id);
    hu_error_t err = hu_cron_remove_job(sched, job_id);
    if (err != HU_OK) {
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("job not found", 14);
        return HU_OK;
    }
    char *msg = hu_sprintf(alloc, "{\"removed\":true,\"id\":\"%llu\"}", (unsigned long long)job_id);
    hu_cron_destroy(sched, alloc);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = hu_tool_result_fail("cron_remove: scheduler not configured", 38);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;
    hu_error_t err = hu_cron_remove_job(sched, job_id);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("job not found", 14);
        return HU_OK;
    }
    char *msg = hu_sprintf(alloc, "{\"removed\":true,\"id\":\"%llu\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#endif
}

static const char *cron_remove_name(void *ctx) {
    (void)ctx;
    return HU_CRON_REMOVE_NAME;
}
static const char *cron_remove_description(void *ctx) {
    (void)ctx;
    return HU_CRON_REMOVE_DESC;
}
static const char *cron_remove_parameters_json(void *ctx) {
    (void)ctx;
    return HU_CRON_REMOVE_PARAMS;
}
static void cron_remove_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_cron_tool_ctx_t));
}

static const hu_tool_vtable_t cron_remove_vtable = {
    .execute = cron_remove_execute,
    .name = cron_remove_name,
    .description = cron_remove_description,
    .parameters_json = cron_remove_parameters_json,
    .deinit = cron_remove_deinit,
};

hu_error_t hu_cron_remove_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched,
                                 hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_cron_tool_ctx_t *ctx =
        (hu_cron_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_cron_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_cron_tool_ctx_t));
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_remove_vtable;
    return HU_OK;
}
