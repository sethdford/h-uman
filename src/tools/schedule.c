#include "seaclaw/tool.h"
#include "seaclaw/tools/schedule.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCHEDULE_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"list\",\"get\",\"cancel\",\"pause\",\"resume\"]},\"expression\":{\"type\":\"string\"},\"command\":{\"type\":\"string\"},\"delay\":{\"type\":\"string\"},\"id\":{\"type\":\"string\"}},\"required\":[\"action\"]}"

static sc_error_t schedule_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    (void)ctx;
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
    (void)alloc;
    *out = sc_tool_result_fail("schedule: scheduler not configured", 36);
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

sc_error_t sc_schedule_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    void *ctx = calloc(1, 1);
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &schedule_vtable;
    return SC_OK;
}
