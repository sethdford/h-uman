#include "seaclaw/tool.h"
#include "seaclaw/tools/schedule.h"
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCHEDULE_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"list\",\"get\",\"cancel\",\"pause\",\"resume\"]},\"expression\":{\"type\":\"string\"},\"command\":{\"type\":\"string\"},\"delay\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"}},\"required\":[\"action\"]}"

typedef struct { sc_cron_scheduler_t *sched; } sc_schedule_tool_ctx_t;

static sc_error_t schedule_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_schedule_tool_ctx_t *tctx = (sc_schedule_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'action' parameter", 27);
        return SC_OK;
    }
#if SC_IS_TEST
    if (strcmp(action, "list") == 0) {
        char *msg = sc_strndup(alloc, "No scheduled tasks.", 19);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 19);
        return SC_OK;
    }
    if (strcmp(action, "create") == 0) {
        const char *expression = sc_json_get_string(args, "expression");
        const char *command = sc_json_get_string(args, "command");
        const char *expr = expression && expression[0] ? expression : "* * * * *";
        const char *cmd = command && command[0] ? command : "";
        size_t need = 64 + strlen(expr) + strlen(cmd);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, need + 1, "{\"id\":\"1\",\"expression\":\"%s\",\"command\":\"%s\"}", expr, cmd);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
    }
    if (strcmp(action, "get") == 0) {
        char *msg = sc_strndup(alloc, "{\"id\":\"1\",\"status\":\"pending\"}", 29);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 29);
        return SC_OK;
    }
    if (strcmp(action, "cancel") == 0 || strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0) {
        char *msg = sc_strndup(alloc, "{\"status\":\"ok\"}", 15);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 15);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = sc_tool_result_fail("schedule: scheduler not configured", 36);
        return SC_OK;
    }
    sc_cron_scheduler_t *sched = tctx->sched;

    if (strcmp(action, "create") == 0) {
        const char *expression = sc_json_get_string(args, "expression");
        const char *command = sc_json_get_string(args, "command");
        const char *name = sc_json_get_string(args, "name");
        const char *expr = expression && expression[0] ? expression : "* * * * *";
        const char *cmd = command && command[0] ? command : "";
        uint64_t id = 0;
        sc_error_t err = sc_cron_add_job(sched, alloc, expr, cmd, name, &id);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("failed to add job", 18);
            return err;
        }
        char *msg = sc_sprintf(alloc, "{\"id\":\"%llu\",\"expression\":\"%s\",\"command\":\"%s\"}",
            (unsigned long long)id, expr, cmd);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        *out = sc_tool_result_ok_owned(msg, strlen(msg));
        return SC_OK;
    }
    if (strcmp(action, "list") == 0) {
        size_t count = 0;
        const sc_cron_job_t *jobs = sc_cron_list_jobs(sched, &count);
        if (count == 0) {
            char *msg = sc_strndup(alloc, "No scheduled tasks.", 19);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, 19);
            return SC_OK;
        }
        sc_json_buf_t buf;
        if (sc_json_buf_init(&buf, alloc) != SC_OK) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) goto list_fail;
        for (size_t i = 0; i < count; i++) {
            if (i > 0) sc_json_buf_append_raw(&buf, ",", 1);
            const sc_cron_job_t *j = &jobs[i];
            if (sc_json_buf_append_raw(&buf, "{\"id\":", 6) != SC_OK) goto list_fail;
            char nbuf[32];
            int nlen = snprintf(nbuf, sizeof(nbuf), "\"%llu\"", (unsigned long long)j->id);
            sc_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
            if (j->expression) {
                sc_json_buf_append_raw(&buf, ",\"expression\":", 14);
                sc_json_append_string(&buf, j->expression, strlen(j->expression));
            }
            if (j->command) {
                sc_json_buf_append_raw(&buf, ",\"command\":", 11);
                sc_json_append_string(&buf, j->command, strlen(j->command));
            }
            sc_json_buf_append_raw(&buf, "}", 1);
        }
        sc_json_buf_append_raw(&buf, "]", 1);
        {
            char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
            if (!msg) { list_fail: sc_json_buf_free(&buf); *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
            memcpy(msg, buf.ptr, buf.len);
            msg[buf.len] = '\0';
            sc_json_buf_free(&buf);
            *out = sc_tool_result_ok_owned(msg, buf.len);
        }
        return SC_OK;
    }
    if (strcmp(action, "get") == 0 || strcmp(action, "cancel") == 0 ||
        strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0) {
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

        if (strcmp(action, "get") == 0) {
            const sc_cron_job_t *job = sc_cron_get_job(sched, job_id);
            if (!job) {
                *out = sc_tool_result_fail("job not found", 14);
                return SC_OK;
            }
            char *msg = sc_sprintf(alloc, "{\"id\":\"%llu\",\"expression\":\"%s\",\"command\":\"%s\",\"status\":\"%s\"}",
                (unsigned long long)job->id,
                job->expression ? job->expression : "",
                job->command ? job->command : "",
                job->enabled ? "enabled" : "paused");
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, strlen(msg));
            return SC_OK;
        }
        if (strcmp(action, "cancel") == 0) {
            sc_error_t err = sc_cron_remove_job(sched, job_id);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("job not found", 14);
                return SC_OK;
            }
            char *msg = sc_strndup(alloc, "{\"status\":\"ok\"}", 15);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, 15);
            return SC_OK;
        }
        if (strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0) {
            bool enabled = (strcmp(action, "resume") == 0);
            sc_error_t err = sc_cron_update_job(sched, alloc, job_id, NULL, NULL, &enabled);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("update failed", 13);
                return err;
            }
            char *msg = sc_strndup(alloc, "{\"status\":\"ok\"}", 15);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            *out = sc_tool_result_ok_owned(msg, 15);
            return SC_OK;
        }
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#endif
}

static const char *schedule_name(void *ctx) { (void)ctx; return "schedule"; }
static const char *schedule_desc(void *ctx) {
    (void)ctx;
    return "Manage scheduled tasks: create, list, get, cancel, pause, resume.";
}
static const char *schedule_params(void *ctx) { (void)ctx; return SCHEDULE_PARAMS; }
static void schedule_deinit(void *ctx, sc_allocator_t *alloc) { (void)ctx; (void)alloc; free(ctx); }

static const sc_tool_vtable_t schedule_vtable = {
    .execute = schedule_execute, .name = schedule_name,
    .description = schedule_desc, .parameters_json = schedule_params,
    .deinit = schedule_deinit,
};

sc_error_t sc_schedule_create(sc_allocator_t *alloc, sc_cron_scheduler_t *sched, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_schedule_tool_ctx_t *ctx = (sc_schedule_tool_ctx_t *)calloc(1, sizeof(sc_schedule_tool_ctx_t));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &schedule_vtable;
    return SC_OK;
}
