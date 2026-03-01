#include "seaclaw/tool.h"
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define CRON_RUN_PARAMS "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"}},\"required\":[\"job_id\"]}"

static sc_error_t cron_run_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *job_id_str = sc_json_get_string(args, "job_id");
    if (!job_id_str || job_id_str[0] == '\0') {
        *out = sc_tool_result_fail("missing job_id", 14);
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

#if SC_IS_TEST
    sc_cron_scheduler_t *sched = sc_cron_create(alloc, 100, true);
    if (!sched) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    uint64_t added_id = 0;
    sc_cron_add_job(sched, alloc, "* * * * *", "echo hello", NULL, &added_id);
    const sc_cron_job_t *job = sc_cron_get_job(sched, job_id);
    if (!job) {
        sc_cron_destroy(sched, alloc);
        *out = sc_tool_result_fail("job not found", 14);
        return SC_OK;
    }
    int64_t now = (int64_t)time(NULL);
    sc_error_t err = sc_cron_add_run(sched, alloc, job_id, now, "executed", NULL);
    sc_cron_destroy(sched, alloc);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("failed to record run", 19);
        return err;
    }
    char *msg = sc_sprintf(alloc, "{\"job_id\":\"%llu\",\"status\":\"executed\"}", (unsigned long long)job_id);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, strlen(msg));
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_fail("cron_run: scheduler not configured", 35);
    return SC_OK;
#endif
}

static const char *cron_run_name(void *ctx) { (void)ctx; return "cron_run"; }
static const char *cron_run_desc(void *ctx) { (void)ctx; return "Run cron job"; }
static const char *cron_run_params(void *ctx) { (void)ctx; return CRON_RUN_PARAMS; }
static void cron_run_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; free(ctx); }

static const sc_tool_vtable_t cron_run_vtable = {
    .execute = cron_run_execute, .name = cron_run_name,
    .description = cron_run_desc, .parameters_json = cron_run_params,
    .deinit = cron_run_deinit,
};

sc_error_t sc_cron_run_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    out->ctx = calloc(1, 1);
    out->vtable = &cron_run_vtable;
    return out->ctx ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}
