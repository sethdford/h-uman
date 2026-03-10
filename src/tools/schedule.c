#include "human/tools/schedule.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHEDULE_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\"," \
    "\"list\",\"get\",\"cancel\",\"pause\",\"resume\"]},\"expression\":{\"type\":\"string\"},"   \
    "\"command\":{\"type\":\"string\"},\"delay\":{\"type\":\"string\"},\"id\":{\"type\":"        \
    "\"string\"},\"type\":{\"type\":\"string\",\"enum\":[\"shell\",\"agent\"]},"                 \
    "\"prompt\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"}"                         \
    "},\"required\":[\"action\"]}"

typedef struct {
    hu_cron_scheduler_t *sched;
} hu_schedule_tool_ctx_t;

static hu_error_t schedule_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_schedule_tool_ctx_t *tctx = (hu_schedule_tool_ctx_t *)ctx;
    (void)tctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = hu_tool_result_fail("Missing 'action' parameter", 27);
        return HU_OK;
    }
#if HU_IS_TEST
    if (strcmp(action, "list") == 0) {
        char *msg = hu_strndup(alloc, "No scheduled tasks.", 19);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 19);
        return HU_OK;
    }
    if (strcmp(action, "create") == 0) {
        const char *expression = hu_json_get_string(args, "expression");
        const char *command = hu_json_get_string(args, "command");
        const char *expr = expression && expression[0] ? expression : "* * * * *";
        const char *cmd = command && command[0] ? command : "";
        size_t need = 64 + strlen(expr) + strlen(cmd);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "{\"id\":\"1\",\"expression\":\"%s\",\"command\":\"%s\"}",
                         expr, cmd);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = hu_tool_result_ok_owned(msg, len);
        return HU_OK;
    }
    if (strcmp(action, "get") == 0) {
        char *msg = hu_strndup(alloc, "{\"id\":\"1\",\"status\":\"pending\"}", 29);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 29);
        return HU_OK;
    }
    if (strcmp(action, "cancel") == 0 || strcmp(action, "pause") == 0 ||
        strcmp(action, "resume") == 0) {
        char *msg = hu_strndup(alloc, "{\"status\":\"ok\"}", 15);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 15);
        return HU_OK;
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#else
    if (!tctx || !tctx->sched) {
        *out = hu_tool_result_fail("schedule: scheduler not configured", 36);
        return HU_OK;
    }
    hu_cron_scheduler_t *sched = tctx->sched;

    if (strcmp(action, "create") == 0) {
        const char *expression = hu_json_get_string(args, "expression");
        const char *name = hu_json_get_string(args, "name");
        const char *type_str = hu_json_get_string(args, "type");
        const char *expr = expression && expression[0] ? expression : "* * * * *";
        uint64_t id = 0;
        hu_error_t err;

        if (type_str && strcmp(type_str, "agent") == 0) {
            const char *prompt = hu_json_get_string(args, "prompt");
            const char *channel = hu_json_get_string(args, "channel");
            if (!prompt || !prompt[0]) {
                *out = hu_tool_result_fail("agent job requires 'prompt'", 27);
                return HU_OK;
            }
            err = hu_cron_add_agent_job(sched, alloc, expr, prompt, channel, name, &id);
        } else {
            const char *command = hu_json_get_string(args, "command");
            const char *cmd = command && command[0] ? command : "";
            err = hu_cron_add_job(sched, alloc, expr, cmd, name, &id);
        }
        if (err != HU_OK) {
            *out = hu_tool_result_fail("failed to add job", 18);
            return err;
        }
        char *msg = hu_sprintf(alloc, "{\"id\":\"%llu\",\"expression\":\"%s\",\"type\":\"%s\"}",
                               (unsigned long long)id, expr,
                               (type_str && strcmp(type_str, "agent") == 0) ? "agent" : "shell");
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, strlen(msg));
        return HU_OK;
    }
    if (strcmp(action, "list") == 0) {
        size_t count = 0;
        const hu_cron_job_t *jobs = hu_cron_list_jobs(sched, &count);
        if (count == 0) {
            char *msg = hu_strndup(alloc, "No scheduled tasks.", 19);
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 12);
                return HU_ERR_OUT_OF_MEMORY;
            }
            *out = hu_tool_result_ok_owned(msg, 19);
            return HU_OK;
        }
        hu_json_buf_t buf;
        if (hu_json_buf_init(&buf, alloc) != HU_OK) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (hu_json_buf_append_raw(&buf, "[", 1) != HU_OK)
            goto list_fail;
        for (size_t i = 0; i < count; i++) {
            if (i > 0)
                hu_json_buf_append_raw(&buf, ",", 1);
            const hu_cron_job_t *j = &jobs[i];
            if (hu_json_buf_append_raw(&buf, "{\"id\":", 6) != HU_OK)
                goto list_fail;
            char nbuf[32];
            int nlen = snprintf(nbuf, sizeof(nbuf), "\"%llu\"", (unsigned long long)j->id);
            hu_json_buf_append_raw(&buf, nbuf, (size_t)nlen);
            if (j->expression) {
                hu_json_buf_append_raw(&buf, ",\"expression\":", 14);
                hu_json_append_string(&buf, j->expression, strlen(j->expression));
            }
            if (j->command) {
                hu_json_buf_append_raw(&buf, ",\"command\":", 11);
                hu_json_append_string(&buf, j->command, strlen(j->command));
            }
            hu_json_buf_append_raw(&buf, "}", 1);
        }
        hu_json_buf_append_raw(&buf, "]", 1);
        {
            char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
            if (!msg) {
            list_fail:
                hu_json_buf_free(&buf);
                *out = hu_tool_result_fail("out of memory", 12);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(msg, buf.ptr, buf.len);
            msg[buf.len] = '\0';
            hu_json_buf_free(&buf);
            *out = hu_tool_result_ok_owned(msg, buf.len);
        }
        return HU_OK;
    }
    if (strcmp(action, "get") == 0 || strcmp(action, "cancel") == 0 ||
        strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0) {
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

        if (strcmp(action, "get") == 0) {
            const hu_cron_job_t *job = hu_cron_get_job(sched, job_id);
            if (!job) {
                *out = hu_tool_result_fail("job not found", 14);
                return HU_OK;
            }
            char *msg = hu_sprintf(
                alloc,
                "{\"id\":\"%llu\",\"expression\":\"%s\",\"command\":\"%s\",\"status\":\"%s\"}",
                (unsigned long long)job->id, job->expression ? job->expression : "",
                job->command ? job->command : "", job->enabled ? "enabled" : "paused");
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 12);
                return HU_ERR_OUT_OF_MEMORY;
            }
            *out = hu_tool_result_ok_owned(msg, strlen(msg));
            return HU_OK;
        }
        if (strcmp(action, "cancel") == 0) {
            hu_error_t err = hu_cron_remove_job(sched, job_id);
            if (err != HU_OK) {
                *out = hu_tool_result_fail("job not found", 14);
                return HU_OK;
            }
            char *msg = hu_strndup(alloc, "{\"status\":\"ok\"}", 15);
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 12);
                return HU_ERR_OUT_OF_MEMORY;
            }
            *out = hu_tool_result_ok_owned(msg, 15);
            return HU_OK;
        }
        if (strcmp(action, "pause") == 0 || strcmp(action, "resume") == 0) {
            bool enabled = (strcmp(action, "resume") == 0);
            hu_error_t err = hu_cron_update_job(sched, alloc, job_id, NULL, NULL, &enabled);
            if (err != HU_OK) {
                *out = hu_tool_result_fail("update failed", 13);
                return err;
            }
            char *msg = hu_strndup(alloc, "{\"status\":\"ok\"}", 15);
            if (!msg) {
                *out = hu_tool_result_fail("out of memory", 12);
                return HU_ERR_OUT_OF_MEMORY;
            }
            *out = hu_tool_result_ok_owned(msg, 15);
            return HU_OK;
        }
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#endif
}

static const char *schedule_name(void *ctx) {
    (void)ctx;
    return "schedule";
}
static const char *schedule_desc(void *ctx) {
    (void)ctx;
    return "Manage scheduled tasks: create, list, get, cancel, pause, resume.";
}
static const char *schedule_params(void *ctx) {
    (void)ctx;
    return SCHEDULE_PARAMS;
}
static void schedule_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_schedule_tool_ctx_t));
}

static const hu_tool_vtable_t schedule_vtable = {
    .execute = schedule_execute,
    .name = schedule_name,
    .description = schedule_desc,
    .parameters_json = schedule_params,
    .deinit = schedule_deinit,
};

hu_error_t hu_schedule_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_schedule_tool_ctx_t *ctx =
        (hu_schedule_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_schedule_tool_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(hu_schedule_tool_ctx_t));
    ctx->sched = sched;
    out->ctx = ctx;
    out->vtable = &schedule_vtable;
    return HU_OK;
}
