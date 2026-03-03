#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOOL_NAME "workflow"
#define TOOL_DESC                                                                                   \
    "Multi-step workflow engine with approval gates. Actions: create (define workflow with "        \
    "ordered steps), run (execute a workflow), status (check workflow progress), approve "          \
    "(approve a pending step), list (show all workflows), cancel (abort a running workflow)."
#define TOOL_PARAMS                                                                                \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\","   \
    "\"run\",\"status\",\"approve\",\"list\",\"cancel\"]},\"workflow_id\":{\"type\":\"string\"},"  \
    "\"name\":{\"type\":\"string\"},\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"object\"," \
    "\"properties\":{\"name\":{\"type\":\"string\"},\"tool\":{\"type\":\"string\"},"               \
    "\"args\":{\"type\":\"object\"},\"requires_approval\":{\"type\":\"boolean\"}}}},\"step_id\""   \
    ":{\"type\":\"string\"},\"trigger\":{\"type\":\"string\",\"description\":"                     \
    "\"cron expression or event name for auto-trigger\"}},\"required\":[\"action\"]}"

#define WF_MAX       16
#define WF_STEPS_MAX 32

typedef enum {
    WF_STATUS_CREATED,
    WF_STATUS_RUNNING,
    WF_STATUS_WAITING_APPROVAL,
    WF_STATUS_COMPLETED,
    WF_STATUS_CANCELLED,
    WF_STATUS_FAILED
} wf_status_t;

typedef struct {
    char name[128];
    char tool[64];
    bool requires_approval;
    bool completed;
    bool approved;
} wf_step_t;

typedef struct {
    char id[32];
    char name[128];
    wf_step_t steps[WF_STEPS_MAX];
    size_t step_count;
    size_t current_step;
    wf_status_t status;
} wf_def_t;

typedef struct {
    sc_allocator_t *alloc;
    wf_def_t workflows[WF_MAX];
    size_t count;
    uint32_t next_id;
} workflow_ctx_t;

static const char *wf_status_str(wf_status_t s) {
    switch (s) {
    case WF_STATUS_CREATED: return "created";
    case WF_STATUS_RUNNING: return "running";
    case WF_STATUS_WAITING_APPROVAL: return "waiting_approval";
    case WF_STATUS_COMPLETED: return "completed";
    case WF_STATUS_CANCELLED: return "cancelled";
    case WF_STATUS_FAILED: return "failed";
    }
    return "unknown";
}

static wf_def_t *wf_find(workflow_ctx_t *c, const char *id) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->workflows[i].id, id) == 0)
            return &c->workflows[i];
    return NULL;
}

static sc_error_t workflow_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                   sc_tool_result_t *out) {
    workflow_ctx_t *c = (workflow_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action) {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }

    if (strcmp(action, "create") == 0) {
        if (c->count >= WF_MAX) {
            *out = sc_tool_result_fail("workflow limit reached", 21);
            return SC_OK;
        }
        const char *name = sc_json_get_string(args, "name");
        wf_def_t *wf = &c->workflows[c->count];
        memset(wf, 0, sizeof(*wf));
        snprintf(wf->id, sizeof(wf->id), "wf_%u", c->next_id++);
        if (name) {
            size_t nl = strlen(name);
            if (nl > sizeof(wf->name) - 1) nl = sizeof(wf->name) - 1;
            memcpy(wf->name, name, nl);
            wf->name[nl] = '\0';
        }

        sc_json_value_t *steps = sc_json_object_get((sc_json_value_t *)args, "steps");
        if (steps && steps->type == SC_JSON_ARRAY) {
            for (size_t i = 0; i < steps->data.array.len && wf->step_count < WF_STEPS_MAX; i++) {
                sc_json_value_t *s = steps->data.array.items[i];
                if (!s) continue;
                wf_step_t *step = &wf->steps[wf->step_count];
                memset(step, 0, sizeof(*step));
                const char *sname = sc_json_get_string(s, "name");
                const char *stool = sc_json_get_string(s, "tool");
                if (sname) {
                    size_t sl = strlen(sname);
                    if (sl > sizeof(step->name) - 1) sl = sizeof(step->name) - 1;
                    memcpy(step->name, sname, sl);
                }
                if (stool) {
                    size_t tl = strlen(stool);
                    if (tl > sizeof(step->tool) - 1) tl = sizeof(step->tool) - 1;
                    memcpy(step->tool, stool, tl);
                }
                sc_json_value_t *ra = sc_json_object_get(s, "requires_approval");
                step->requires_approval = (ra && ra->type == SC_JSON_BOOL && ra->data.boolean);
                wf->step_count++;
            }
        }
        wf->status = WF_STATUS_CREATED;
        c->count++;
        char *msg = sc_sprintf(alloc, "{\"created\":true,\"workflow_id\":\"%s\",\"steps\":%zu}",
                               wf->id, wf->step_count);
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
        return SC_OK;
    }

    if (strcmp(action, "run") == 0) {
        const char *wf_id = sc_json_get_string(args, "workflow_id");
        if (!wf_id) { *out = sc_tool_result_fail("missing workflow_id", 18); return SC_OK; }
        wf_def_t *wf = wf_find(c, wf_id);
        if (!wf) { *out = sc_tool_result_fail("workflow not found", 18); return SC_OK; }
        if (wf->status != WF_STATUS_CREATED) {
            *out = sc_tool_result_fail("workflow already started", 24);
            return SC_OK;
        }
        wf->status = WF_STATUS_RUNNING;
        wf->current_step = 0;
        while (wf->current_step < wf->step_count) {
            wf_step_t *step = &wf->steps[wf->current_step];
            if (step->requires_approval && !step->approved) {
                wf->status = WF_STATUS_WAITING_APPROVAL;
                char *msg = sc_sprintf(alloc,
                    "{\"status\":\"waiting_approval\",\"step\":\"%s\",\"step_index\":%zu}",
                    step->name, wf->current_step);
                *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
                return SC_OK;
            }
            step->completed = true;
            wf->current_step++;
        }
        wf->status = WF_STATUS_COMPLETED;
        *out = sc_tool_result_ok("{\"status\":\"completed\"}", 23);
        return SC_OK;
    }

    if (strcmp(action, "approve") == 0) {
        const char *wf_id = sc_json_get_string(args, "workflow_id");
        if (!wf_id) { *out = sc_tool_result_fail("missing workflow_id", 18); return SC_OK; }
        wf_def_t *wf = wf_find(c, wf_id);
        if (!wf) { *out = sc_tool_result_fail("workflow not found", 18); return SC_OK; }
        if (wf->status != WF_STATUS_WAITING_APPROVAL) {
            *out = sc_tool_result_fail("workflow not waiting for approval", 32);
            return SC_OK;
        }
        wf->steps[wf->current_step].approved = true;
        wf->steps[wf->current_step].completed = true;
        wf->current_step++;
        while (wf->current_step < wf->step_count) {
            wf_step_t *step = &wf->steps[wf->current_step];
            if (step->requires_approval && !step->approved) {
                wf->status = WF_STATUS_WAITING_APPROVAL;
                char *msg = sc_sprintf(alloc,
                    "{\"approved\":true,\"next_step\":\"%s\",\"status\":\"waiting_approval\"}",
                    step->name);
                *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
                return SC_OK;
            }
            step->completed = true;
            wf->current_step++;
        }
        wf->status = WF_STATUS_COMPLETED;
        *out = sc_tool_result_ok("{\"approved\":true,\"status\":\"completed\"}", 40);
        return SC_OK;
    }

    if (strcmp(action, "status") == 0) {
        const char *wf_id = sc_json_get_string(args, "workflow_id");
        if (!wf_id) { *out = sc_tool_result_fail("missing workflow_id", 18); return SC_OK; }
        wf_def_t *wf = wf_find(c, wf_id);
        if (!wf) { *out = sc_tool_result_fail("workflow not found", 18); return SC_OK; }
        char *msg = sc_sprintf(alloc,
            "{\"workflow_id\":\"%s\",\"name\":\"%s\",\"status\":\"%s\","
            "\"current_step\":%zu,\"total_steps\":%zu}",
            wf->id, wf->name, wf_status_str(wf->status),
            wf->current_step, wf->step_count);
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
        return SC_OK;
    }

    if (strcmp(action, "list") == 0) {
        size_t buf_sz = 256 + c->count * 256;
        char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 13); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, buf_sz, "{\"workflows\":[");
        for (size_t i = 0; i < c->count; i++) {
            wf_def_t *wf = &c->workflows[i];
            n += snprintf(msg + n, buf_sz - (size_t)n,
                          "%s{\"id\":\"%s\",\"name\":\"%s\",\"status\":\"%s\",\"steps\":%zu}",
                          i > 0 ? "," : "", wf->id, wf->name,
                          wf_status_str(wf->status), wf->step_count);
        }
        n += snprintf(msg + n, buf_sz - (size_t)n, "]}");
        *out = sc_tool_result_ok_owned(msg, (size_t)n);
        return SC_OK;
    }

    if (strcmp(action, "cancel") == 0) {
        const char *wf_id = sc_json_get_string(args, "workflow_id");
        if (!wf_id) { *out = sc_tool_result_fail("missing workflow_id", 18); return SC_OK; }
        wf_def_t *wf = wf_find(c, wf_id);
        if (!wf) { *out = sc_tool_result_fail("workflow not found", 18); return SC_OK; }
        wf->status = WF_STATUS_CANCELLED;
        *out = sc_tool_result_ok("{\"cancelled\":true}", 19);
        return SC_OK;
    }

    *out = sc_tool_result_fail("unknown action", 14);
    return SC_OK;
}

static const char *workflow_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *workflow_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *workflow_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void workflow_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t workflow_vtable = {
    .execute = workflow_execute, .name = workflow_name, .description = workflow_desc,
    .parameters_json = workflow_params, .deinit = workflow_deinit,
};

sc_error_t sc_workflow_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    workflow_ctx_t *c = (workflow_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    out->ctx = c;
    out->vtable = &workflow_vtable;
    return SC_OK;
}
