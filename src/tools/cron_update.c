#include "seaclaw/tool.h"
#include <stdint.h>
#include "seaclaw/tools/cron_update.h"
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

#define CRON_UPDATE_PARAMS "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"expression\":{\"type\":\"string\"},\"command\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"job_id\"]}"

typedef struct { sc_cron_scheduler_t *sched; } sc_cron_tool_ctx_t;

static sc_error_t cron_update_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_cron_tool_ctx_t *tctx = (sc_cron_tool_ctx_t *)ctx;
    (void)tctx;
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
    const char *expr = sc_json_get_string(args, "expression");
    const char *cmd = sc_json_get_string(args, "command");
    bool enabled_val = sc_json_get_bool(args, "enabled", true);
    const bool *enabled_ptr = (sc_json_object_get(args, "enabled") != NULL) ? &enabled_val : NULL;
    if (!expr && !cmd && !enabled_ptr) {
        *out = sc_tool_result_fail("Nothing to update — provide expression, command, or enabled", 55);
        return SC_OK;
    }

#if SC_IS_TEST
    sc_cron_scheduler_t *sched = sc_cron_create(alloc, 100, true);
    if (!sched) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    sc_cron_add_job(sched, alloc, "* * * * *", "echo old", NULL, &added_id);
    if (added_id != job_id) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("job not found", 14);
        return SC_OK;
    }
    sc_error_t err = sc_cron_update_job(sched, alloc, job_id, expr, cmd, enabled_ptr);
    sc_cron_destroy(sched, alloc);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("update failed", 13);
        return err;
    }
    char *msg = sc_sprintf(alloc, "{\"updated\":true,\"job_id\":\"%llu\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, strlen(msg));
    return SC_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = sc_tool_result_fail("cron_update: scheduler not configured", 38);
        return SC_OK;
    }
    sc_cron_scheduler_t *sched = tctx->sched;
    sc_error_t err = sc_cron_update_job(sched, alloc, job_id, expr, cmd, enabled_ptr);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("update failed", 13);
        return err;
    }
    char *msg = sc_sprintf(alloc, "{\"updated\":true,\"job_id\":\"%llu\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, strlen(msg));
    return SC_OK;
#endif
}

static const char *cron_update_name(void *ctx) { (void)ctx; return "cron_update"; }
static const char *cron_update_desc(void *ctx) {
    (void)ctx;
    return "Update a cron job: change expression, command, or enable/disable it.";
}
static const char *cron_update_params(void *ctx) { (void)ctx; return CRON_UPDATE_PARAMS; }
static void cron_update_deinit(void *ctx, sc_allocator_t *alloc) { (void)ctx; (void)alloc; free(ctx); }

static const sc_tool_vtable_t cron_update_vtable = {
    .execute = cron_update_execute, .name = cron_update_name,
    .description = cron_update_desc, .parameters_json = cron_update_params,
    .deinit = cron_update_deinit,
};

sc_error_t sc_cron_update_create(sc_allocator_t *alloc, sc_cron_scheduler_t *sched, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_cron_tool_ctx_t *ctx = (sc_cron_tool_ctx_t *)calloc(1, sizeof(sc_cron_tool_ctx_t));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_update_vtable;
    return SC_OK;
}
