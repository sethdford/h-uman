#include "human/tool.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/paperclip/client.h"
#include <stdio.h>
#include <string.h>

typedef struct paperclip_ctx {
    hu_allocator_t *alloc;
    hu_paperclip_client_t client;
    bool initialized;
} paperclip_ctx_t;

static hu_error_t ensure_client(paperclip_ctx_t *ctx) {
    if (ctx->initialized)
        return HU_OK;
    hu_error_t err = hu_paperclip_client_init(&ctx->client, ctx->alloc);
    if (err == HU_OK)
        ctx->initialized = true;
    return err;
}

static hu_error_t paperclip_execute(void *raw_ctx, hu_allocator_t *alloc,
                                     const hu_json_value_t *args,
                                     hu_tool_result_t *out) {
    paperclip_ctx_t *ctx = (paperclip_ctx_t *)raw_ctx;
    if (!ctx || !args || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        out->success = false;
        out->output = hu_strdup(alloc, "Missing required 'action' parameter");
        out->output_len = out->output ? strlen(out->output) : 0;
        return HU_OK;
    }

    if (strcmp(action, "list_tasks") != 0 && strcmp(action, "update_status") != 0 &&
        strcmp(action, "comment") != 0 && strcmp(action, "get_task") != 0) {
        out->success = false;
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Unknown action '%s'. Use: list_tasks, update_status, comment, get_task", action);
        out->output = hu_strdup(alloc, msg);
        out->output_len = out->output ? strlen(out->output) : 0;
        return HU_OK;
    }

    hu_error_t err = ensure_client(ctx);
    if (err != HU_OK) {
        out->success = false;
        out->output = hu_strdup(alloc, "Paperclip client not configured. "
                                        "Set PAPERCLIP_API_URL and PAPERCLIP_AGENT_ID.");
        out->output_len = out->output ? strlen(out->output) : 0;
        return HU_OK;
    }

    if (strcmp(action, "list_tasks") == 0) {
        hu_paperclip_task_list_t list = {0};
        err = hu_paperclip_list_tasks(&ctx->client, &list);
        if (err != HU_OK) {
            out->success = false;
            out->output = hu_strdup(alloc, "Failed to list tasks");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }

        char *buf = (char *)alloc->alloc(alloc->ctx, 4096);
        if (!buf) {
            hu_paperclip_task_list_free(alloc, &list);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, 4096 - pos, "Found %zu task(s):\n", list.count);
        for (size_t i = 0; i < list.count && pos < 3900; i++) {
            pos += (size_t)snprintf(buf + pos, 4096 - pos, "- [%s] %s (status: %s)\n",
                                    list.tasks[i].id ? list.tasks[i].id : "?",
                                    list.tasks[i].title ? list.tasks[i].title : "(untitled)",
                                    list.tasks[i].status ? list.tasks[i].status : "?");
        }
        out->success = true;
        out->output = buf;
        out->output_len = pos;
        hu_paperclip_task_list_free(alloc, &list);
        return HU_OK;
    }

    if (strcmp(action, "update_status") == 0) {
        const char *task_id = hu_json_get_string(args, "task_id");
        const char *status = hu_json_get_string(args, "status");
        if (!task_id || !status) {
            out->success = false;
            out->output = hu_strdup(alloc, "Missing 'task_id' or 'status'");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }
        err = hu_paperclip_update_task(&ctx->client, task_id, status);
        if (err != HU_OK) {
            out->success = false;
            out->output = hu_strdup(alloc, "Failed to update task status");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "Task %s updated to '%s'", task_id, status);
        out->success = true;
        out->output = hu_strdup(alloc, msg);
        out->output_len = out->output ? strlen(out->output) : 0;
        return HU_OK;
    }

    if (strcmp(action, "comment") == 0) {
        const char *task_id = hu_json_get_string(args, "task_id");
        const char *body = hu_json_get_string(args, "body");
        if (!task_id || !body) {
            out->success = false;
            out->output = hu_strdup(alloc, "Missing 'task_id' or 'body'");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }
        err = hu_paperclip_post_comment(&ctx->client, task_id, body, strlen(body));
        if (err != HU_OK) {
            out->success = false;
            out->output = hu_strdup(alloc, "Failed to post comment");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }
        out->success = true;
        out->output = hu_strdup(alloc, "Comment posted successfully");
        out->output_len = out->output ? strlen(out->output) : 0;
        return HU_OK;
    }

    if (strcmp(action, "get_task") == 0) {
        const char *task_id = hu_json_get_string(args, "task_id");
        if (!task_id) {
            out->success = false;
            out->output = hu_strdup(alloc, "Missing 'task_id'");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }
        hu_paperclip_task_t t = {0};
        err = hu_paperclip_get_task(&ctx->client, task_id, &t);
        if (err != HU_OK) {
            out->success = false;
            out->output = hu_strdup(alloc, "Failed to get task");
            out->output_len = out->output ? strlen(out->output) : 0;
            return HU_OK;
        }
        char *buf = (char *)alloc->alloc(alloc->ctx, 2048);
        if (!buf) {
            hu_paperclip_task_free(alloc, &t);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, 2048 - pos, "Task: %s\n", t.title ? t.title : "?");
        if (t.status)
            pos += (size_t)snprintf(buf + pos, 2048 - pos, "Status: %s\n", t.status);
        if (t.priority)
            pos += (size_t)snprintf(buf + pos, 2048 - pos, "Priority: %s\n", t.priority);
        if (t.description)
            pos += (size_t)snprintf(buf + pos, 2048 - pos, "Description:\n%s\n", t.description);
        out->success = true;
        out->output = buf;
        out->output_len = pos;
        hu_paperclip_task_free(alloc, &t);
        return HU_OK;
    }

    return HU_OK;
}

static const char *paperclip_name(void *ctx) {
    (void)ctx;
    return "paperclip";
}

static const char *paperclip_description(void *ctx) {
    (void)ctx;
    return "Manage tasks in Paperclip. List assigned tasks, update status, post comments.";
}

static const char *paperclip_parameters_json(void *ctx) {
    (void)ctx;
    return "{"
           "\"type\":\"object\","
           "\"properties\":{"
           "\"action\":{\"type\":\"string\",\"enum\":[\"list_tasks\",\"update_status\",\"comment\",\"get_task\"],"
           "\"description\":\"The action to perform\"},"
           "\"task_id\":{\"type\":\"string\",\"description\":\"Task ID (for update_status, comment, get_task)\"},"
           "\"status\":{\"type\":\"string\",\"enum\":[\"todo\",\"in_progress\",\"done\",\"blocked\"],"
           "\"description\":\"New status (for update_status)\"},"
           "\"body\":{\"type\":\"string\",\"description\":\"Comment text (for comment)\"}"
           "},"
           "\"required\":[\"action\"]"
           "}";
}

static void paperclip_deinit(void *raw_ctx, hu_allocator_t *alloc) {
    paperclip_ctx_t *ctx = (paperclip_ctx_t *)raw_ctx;
    if (!ctx)
        return;
    if (ctx->initialized)
        hu_paperclip_client_deinit(&ctx->client);
    alloc->free(alloc->ctx, ctx, sizeof(paperclip_ctx_t));
}

static const hu_tool_vtable_t paperclip_vtable = {
    .execute = paperclip_execute,
    .name = paperclip_name,
    .description = paperclip_description,
    .parameters_json = paperclip_parameters_json,
    .deinit = paperclip_deinit,
};

hu_error_t hu_paperclip_tool_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    paperclip_ctx_t *ctx =
        (paperclip_ctx_t *)alloc->alloc(alloc->ctx, sizeof(paperclip_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;

    out->ctx = ctx;
    out->vtable = &paperclip_vtable;
    return HU_OK;
}
