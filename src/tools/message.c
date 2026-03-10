#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <string.h>

#define HU_MESSAGE_NAME "message"
#define HU_MESSAGE_DESC "Send message to channel"
#define HU_MESSAGE_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\"},\"target\":{\"type\":" \
    "\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"target\",\"content\"]}"
#define HU_MESSAGE_CONTENT_MAX 64000

typedef struct hu_message_ctx {
    hu_channel_t *channel;
} hu_message_ctx_t;

static hu_error_t message_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                  hu_tool_result_t *out) {
    hu_message_ctx_t *c = (hu_message_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *target = hu_json_get_string(args, "target");
    const char *content = hu_json_get_string(args, "content");
    if (!target || strlen(target) == 0) {
        *out = hu_tool_result_fail("missing target", 13);
        return HU_OK;
    }
    if (!content)
        content = "";
    if (strlen(content) > HU_MESSAGE_CONTENT_MAX) {
        *out = hu_tool_result_fail("content too long", 16);
        return HU_OK;
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(message stub)", 14);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 14);
    return HU_OK;
#else
    if (!c->channel || !c->channel->vtable) {
        *out = hu_tool_result_fail("channel not configured", 23);
        return HU_OK;
    }
    hu_error_t err = c->channel->vtable->send(c->channel->ctx, target, strlen(target), content,
                                              strlen(content), NULL, 0);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("send failed", 10);
        return HU_OK;
    }
    char *ok = hu_strndup(alloc, "sent", 4);
    if (!ok) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(ok, 4);
    return HU_OK;
#endif
}

static const char *message_name(void *ctx) {
    (void)ctx;
    return HU_MESSAGE_NAME;
}
static const char *message_description(void *ctx) {
    (void)ctx;
    return HU_MESSAGE_DESC;
}
static const char *message_parameters_json(void *ctx) {
    (void)ctx;
    return HU_MESSAGE_PARAMS;
}
static void message_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_message_ctx_t));
}

static const hu_tool_vtable_t message_vtable = {
    .execute = message_execute,
    .name = message_name,
    .description = message_description,
    .parameters_json = message_parameters_json,
    .deinit = message_deinit,
};

void hu_message_tool_set_channel(hu_tool_t *tool, hu_channel_t *channel) {
    if (!tool || !tool->ctx || tool->vtable != &message_vtable)
        return;
    hu_message_ctx_t *mc = (hu_message_ctx_t *)tool->ctx;
    mc->channel = channel;
}

hu_error_t hu_message_create(hu_allocator_t *alloc, hu_channel_t *channel, hu_tool_t *out) {
    hu_message_ctx_t *c = (hu_message_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->channel = channel;
    out->ctx = c;
    out->vtable = &message_vtable;
    return HU_OK;
}
