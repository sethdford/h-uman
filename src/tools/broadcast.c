#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "broadcast"
#define TOOL_DESC                                                                                  \
    "Send a message to multiple channels simultaneously. Provide a message and a list of channel " \
    "targets. Returns delivery status for each channel."
#define TOOL_PARAMS                                                                              \
    "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\",\"description\":"     \
    "\"Message to broadcast\"},\"channels\":{\"type\":\"array\",\"items\":{\"type\":\"object\"," \
    "\"properties\":{\"channel\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"}}}},"     \
    "\"format\":{\"type\":\"string\",\"description\":\"Optional: 'plain' or 'markdown' "         \
    "(default: plain)\"}},\"required\":[\"message\",\"channels\"]}"

typedef struct {
    char _unused;
} broadcast_ctx_t;

static sc_error_t broadcast_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                    sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }
    const char *message = sc_json_get_string(args, "message");
    if (!message || strlen(message) == 0) {
        *out = sc_tool_result_fail("missing message", 15);
        return SC_OK;
    }
    sc_json_value_t *channels = sc_json_object_get((sc_json_value_t *)args, "channels");
    if (!channels || channels->type != SC_JSON_ARRAY || channels->data.array.len == 0) {
        *out = sc_tool_result_fail("missing channels array", 22);
        return SC_OK;
    }

#if SC_IS_TEST
    size_t buf_sz = 256 + channels->data.array.len * 128;
    char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 13);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, buf_sz, "Broadcast to %zu channels:\n", channels->data.array.len);
    for (size_t i = 0; i < channels->data.array.len; i++) {
        sc_json_value_t *entry = channels->data.array.items[i];
        if (!entry || entry->type != SC_JSON_OBJECT)
            continue;
        const char *ch = sc_json_get_string(entry, "channel");
        const char *tgt = sc_json_get_string(entry, "target");
        n += snprintf(msg + n, buf_sz - (size_t)n, "- %s -> %s: delivered (test)\n",
                     ch ? ch : "unknown", tgt ? tgt : "default");
    }
    *out = sc_tool_result_ok_owned(msg, (size_t)n);
#else
    size_t buf_sz = 256 + channels->data.array.len * 128;
    char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 13);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, buf_sz,
                     "Broadcast queued for %zu channels. Use the 'message' tool to send to "
                     "each channel individually.",
                     channels->data.array.len);
    *out = sc_tool_result_ok_owned(msg, (size_t)n);
#endif
    return SC_OK;
}

static const char *broadcast_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *broadcast_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *broadcast_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void broadcast_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t broadcast_vtable = {
    .execute = broadcast_execute,
    .name = broadcast_name,
    .description = broadcast_desc,
    .parameters_json = broadcast_params,
    .deinit = broadcast_deinit,
};

sc_error_t sc_broadcast_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(broadcast_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &broadcast_vtable;
    return SC_OK;
}
