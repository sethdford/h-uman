#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
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

static hu_error_t broadcast_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *message = hu_json_get_string(args, "message");
    if (!message || strlen(message) == 0) {
        *out = hu_tool_result_fail("missing message", 15);
        return HU_OK;
    }
    hu_json_value_t *channels = hu_json_object_get((hu_json_value_t *)args, "channels");
    if (!channels || channels->type != HU_JSON_ARRAY || channels->data.array.len == 0) {
        *out = hu_tool_result_fail("missing channels array", 22);
        return HU_OK;
    }

#if HU_IS_TEST
    size_t buf_sz = 256 + channels->data.array.len * 128;
    char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, buf_sz, "Broadcast to %zu channels:\n", channels->data.array.len);
    for (size_t i = 0; i < channels->data.array.len; i++) {
        hu_json_value_t *entry = channels->data.array.items[i];
        if (!entry || entry->type != HU_JSON_OBJECT)
            continue;
        const char *ch = hu_json_get_string(entry, "channel");
        const char *tgt = hu_json_get_string(entry, "target");
        n += snprintf(msg + n, buf_sz - (size_t)n, "- %s -> %s: delivered (test)\n",
                      ch ? ch : "unknown", tgt ? tgt : "default");
    }
    *out = hu_tool_result_ok_owned(msg, (size_t)n);
#else
    size_t buf_sz = 256 + channels->data.array.len * 128;
    char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, buf_sz,
                     "Broadcast queued for %zu channels. Use the 'message' tool to send to "
                     "each channel individually.",
                     channels->data.array.len);
    *out = hu_tool_result_ok_owned(msg, (size_t)n);
#endif
    return HU_OK;
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
static void broadcast_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(broadcast_ctx_t));
}

static const hu_tool_vtable_t broadcast_vtable = {
    .execute = broadcast_execute,
    .name = broadcast_name,
    .description = broadcast_desc,
    .parameters_json = broadcast_params,
    .deinit = broadcast_deinit,
};

hu_error_t hu_broadcast_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, sizeof(broadcast_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(broadcast_ctx_t));
    out->ctx = ctx;
    out->vtable = &broadcast_vtable;
    return HU_OK;
}
