#include "human/tools/cron_update.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/tool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CRON_UPDATE_PARAMS                                                                        \
    "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"expression\":{"      \
    "\"type\":\"string\"},\"command\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}}," \
    "\"required\":[\"job_id\"]}"

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_cron_tool_ctx_t;

static hu_error_t cron_update_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
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
    const char *expr = hu_json_get_string(args, "expression");
    const char *cmd = hu_json_get_string(args, "command");
    bool enabled_val = hu_json_get_bool(args, "enabled", true);
    const bool *enabled_ptr = (hu_json_object_get(args, "enabled") != NULL) ? &enabled_val : NULL;
    if (!expr && !cmd && !enabled_ptr) {
        *out =
            hu_tool_result_fail("Nothing to update — provide expression, command, or enabled", 55);
        return HU_OK;
    }

#if HU_IS_TEST
    hu_cron_scheduler_t *sched = hu_cron_create(alloc, 100, true);
    if (!sched) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    hu_cron_add_job(sched, alloc, "* * * * *", "echo old", NULL, &added_id);
    if (added_id != job_id) {
        hu_cron_destroy(sched, alloc);
        *out = hu_tool_result_fail("job not found", 14);
        return HU_OK;
    }
    hu_error_t err = hu_cron_update_job(sched, alloc, job_id, expr, cmd, enabled_ptr);
    hu_cron_destroy(sched, alloc);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("update failed", 13);
        return err;
    }
    char *msg =
        hu_sprintf(alloc, "{\"updated\":true,\"job_id\":\"%llu\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = hu_tool_result_fail("cron_update: scheduler not configured", 38);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;
    hu_error_t err = hu_cron_update_job(sched, alloc, job_id, expr, cmd, enabled_ptr);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("update failed", 13);
        return err;
    }
    char *msg =
        hu_sprintf(alloc, "{\"updated\":true,\"job_id\":\"%llu\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, strlen(msg));
    return HU_OK;
#endif
}

static const char *cron_update_name(void *ctx) {
    (void)ctx;
    return "cron_update";
}
static const char *cron_update_desc(void *ctx) {
    (void)ctx;
    return "Update a cron job: change expression, command, or enable/disable it.";
}
static const char *cron_update_params(void *ctx) {
    (void)ctx;
    return CRON_UPDATE_PARAMS;
}
static void cron_update_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
    free(ctx);
}

static const hu_tool_vtable_t cron_update_vtable = {
    .execute = cron_update_execute,
    .name = cron_update_name,
    .description = cron_update_desc,
    .parameters_json = cron_update_params,
    .deinit = cron_update_deinit,
};

hu_error_t hu_cron_update_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched,
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
    out->vtable = &cron_update_vtable;
    return HU_OK;
}
