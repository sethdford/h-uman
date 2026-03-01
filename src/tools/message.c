#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/json.h"
#include "seaclaw/channel.h"
#include <string.h>
#include <stdlib.h>

#define SC_MESSAGE_NAME "message"
#define SC_MESSAGE_DESC "Send message to channel"
#define SC_MESSAGE_PARAMS "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"target\",\"content\"]}"
#define SC_MESSAGE_CONTENT_MAX 64000

typedef struct sc_message_ctx {
    sc_channel_t *channel;
} sc_message_ctx_t;

static sc_error_t message_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_message_ctx_t *c = (sc_message_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *target = sc_json_get_string(args, "target");
    const char *content = sc_json_get_string(args, "content");
    if (!target || strlen(target) == 0) {
        *out = sc_tool_result_fail("missing target", 13);
        return SC_OK;
    }
    if (!content) content = "";
    if (strlen(content) > SC_MESSAGE_CONTENT_MAX) {
        *out = sc_tool_result_fail("content too long", 16);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(message stub)", 14);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 14);
    return SC_OK;
#else
    if (!c->channel || !c->channel->vtable) {
        *out = sc_tool_result_fail("channel not configured", 23);
        return SC_OK;
    }
    sc_error_t err = c->channel->vtable->send(c->channel->ctx,
        target, strlen(target), content, strlen(content), NULL, 0);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("send failed", 10);
        return SC_OK;
    }
    char *ok = sc_strndup(alloc, "sent", 4);
    if (!ok) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(ok, 4);
    return SC_OK;
#endif
}

static const char *message_name(void *ctx) { (void)ctx; return SC_MESSAGE_NAME; }
static const char *message_description(void *ctx) { (void)ctx; return SC_MESSAGE_DESC; }
static const char *message_parameters_json(void *ctx) { (void)ctx; return SC_MESSAGE_PARAMS; }
static void message_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t message_vtable = {
    .execute = message_execute, .name = message_name,
    .description = message_description, .parameters_json = message_parameters_json,
    .deinit = message_deinit,
};

sc_error_t sc_message_create(sc_allocator_t *alloc,
    sc_channel_t *channel,
    sc_tool_t *out)
{
    (void)alloc;
    sc_message_ctx_t *c = (sc_message_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->channel = channel;
    out->ctx = c;
    out->vtable = &message_vtable;
    return SC_OK;
}
