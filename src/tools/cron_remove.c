#include "seaclaw/tool.h"
#include <stdint.h>
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

#define SC_CRON_REMOVE_NAME "cron_remove"
#define SC_CRON_REMOVE_DESC "Remove cron job"
#define SC_CRON_REMOVE_PARAMS "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}"

typedef struct { sc_cron_scheduler_t *sched; } sc_cron_tool_ctx_t;

static sc_error_t cron_remove_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_cron_tool_ctx_t *tctx = (sc_cron_tool_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *id_str = sc_json_get_string(args, "id");
    if (!id_str || id_str[0] == '\0') {
        *out = sc_tool_result_fail("missing id", 10);
        return SC_OK;
    }
    char *end = NULL;
    unsigned long long id_val = strtoull(id_str, &end, 10);
    if (end == id_str || *end != '\0' || id_val == 0) {
        *out = sc_tool_result_fail("invalid id", 10);
        return SC_OK;
    }
    uint64_t job_id = (uint64_t)id_val;
    (void)job_id;

#if SC_IS_TEST
    sc_cron_scheduler_t *sched = sc_cron_create(alloc, 100, true);
    if (!sched) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    sc_cron_add_job(sched, alloc, "* * * * *", "echo x", NULL, &added_id);
    sc_error_t err = sc_cron_remove_job(sched, job_id);
    if (err != SC_OK) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("job not found", 14);
        return SC_OK;
    }
    char *msg = sc_sprintf(alloc, "{\"removed\":true,\"id\":\"%llu\"}", (unsigned long long)job_id);
    sc_cron_destroy(sched, alloc);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, strlen(msg));
    return SC_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = sc_tool_result_fail("cron_remove: scheduler not configured", 38);
        return SC_OK;
    }
    sc_cron_scheduler_t *sched = tctx->sched;
    sc_error_t err = sc_cron_remove_job(sched, job_id);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("job not found", 14);
        return SC_OK;
    }
    char *msg = sc_sprintf(alloc, "{\"removed\":true,\"id\":\"%llu\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, strlen(msg));
    return SC_OK;
#endif
}

static const char *cron_remove_name(void *ctx) { (void)ctx; return SC_CRON_REMOVE_NAME; }
static const char *cron_remove_description(void *ctx) { (void)ctx; return SC_CRON_REMOVE_DESC; }
static const char *cron_remove_parameters_json(void *ctx) { (void)ctx; return SC_CRON_REMOVE_PARAMS; }
static void cron_remove_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t cron_remove_vtable = {
    .execute = cron_remove_execute, .name = cron_remove_name,
    .description = cron_remove_description, .parameters_json = cron_remove_parameters_json,
    .deinit = cron_remove_deinit,
};

sc_error_t sc_cron_remove_create(sc_allocator_t *alloc, sc_cron_scheduler_t *sched, sc_tool_t *out) {
    (void)alloc;
    sc_cron_tool_ctx_t *ctx = (sc_cron_tool_ctx_t *)calloc(1, sizeof(sc_cron_tool_ctx_t));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &cron_remove_vtable;
    return SC_OK;
}
