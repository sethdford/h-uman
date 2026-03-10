#include "human/agent/mailbox.h"
#include "human/agent/tool_context.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "send_message"
#define TOOL_DESC                                                                             \
    "Send a message to a teammate agent. Use when coordinating with other agents in a team. " \
    "Provide the target agent ID (as a number) and the message content."
#define TOOL_PARAMS                                                                     \
    "{\"type\":\"object\",\"properties\":{"                                             \
    "\"to_agent\":{\"type\":\"number\",\"description\":\"Target agent ID (numeric)\"}," \
    "\"message\":{\"type\":\"string\",\"description\":\"Message content to send\"}"     \
    "},\"required\":[\"to_agent\",\"message\"]}"

typedef struct {
    hu_mailbox_t *mailbox;
} send_message_ctx_t;

static hu_error_t send_message_execute(void *ctx, hu_allocator_t *alloc,
                                       const hu_json_value_t *args, hu_tool_result_t *out) {
    send_message_ctx_t *c = (send_message_ctx_t *)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (!c->mailbox) {
        *out = hu_tool_result_fail("mailbox not configured", 23);
        return HU_OK;
    }

    hu_agent_t *agent = hu_agent_get_current_for_tools();
    if (!agent) {
        *out = hu_tool_result_fail("agent context not available", 28);
        return HU_OK;
    }

    hu_json_value_t *to_val = hu_json_object_get((hu_json_value_t *)args, "to_agent");
    if (!to_val) {
        *out = hu_tool_result_fail("missing to_agent", 15);
        return HU_OK;
    }
    uint64_t to_agent = 0;
    if (to_val->type == HU_JSON_NUMBER)
        to_agent = (uint64_t)to_val->data.number;
    else if (to_val->type == HU_JSON_STRING && to_val->data.string.ptr)
        to_agent = (uint64_t)strtoull(to_val->data.string.ptr, NULL, 10);
    else {
        *out = hu_tool_result_fail("to_agent must be number or numeric string", 38);
        return HU_OK;
    }

    const char *message = hu_json_get_string(args, "message");
    if (!message) {
        *out = hu_tool_result_fail("missing message", 15);
        return HU_OK;
    }

    size_t msg_len = strlen(message);
    uint64_t from_agent = (uint64_t)(uintptr_t)agent;

    hu_error_t err =
        hu_mailbox_send(c->mailbox, from_agent, to_agent, HU_MSG_TASK, message, msg_len, 0);
    if (err != HU_OK) {
        if (err == HU_ERR_NOT_FOUND)
            *out = hu_tool_result_fail("target agent not found", 23);
        else if (err == HU_ERR_OUT_OF_MEMORY)
            *out = hu_tool_result_fail("mailbox full", 12);
        else
            *out = hu_tool_result_fail("send failed", 11);
        return HU_OK;
    }

    char *resp = hu_sprintf(alloc, "Sent to agent %llu: %.*s", (unsigned long long)to_agent,
                            (int)(msg_len > 80 ? 80 : msg_len), message);
    *out = hu_tool_result_ok_owned(resp, resp ? strlen(resp) : 0);
    return HU_OK;
}

static const char *send_message_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *send_message_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *send_message_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void send_message_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(send_message_ctx_t));
}

static const hu_tool_vtable_t send_message_vtable = {
    .execute = send_message_execute,
    .name = send_message_name,
    .description = send_message_desc,
    .parameters_json = send_message_params,
    .deinit = send_message_deinit,
};

hu_error_t hu_send_message_create(hu_allocator_t *alloc, hu_mailbox_t *mailbox, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    send_message_ctx_t *ctx =
        (send_message_ctx_t *)alloc->alloc(alloc->ctx, sizeof(send_message_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mailbox = mailbox;
    out->ctx = ctx;
    out->vtable = &send_message_vtable;
    return HU_OK;
}
