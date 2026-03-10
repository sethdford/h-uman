/*
 * Twitter/X tool — post tweets, search, reply.
 */
#include "human/tools/twitter.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <string.h>

#define HU_TWITTER_NAME "twitter"
#define HU_TWITTER_DESC "Post tweets, search Twitter/X, reply to tweets"
#define HU_TWITTER_PARAMS                                                             \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[" \
    "\"post\",\"search\",\"reply\"]},\"text\":{\"type\":\"string\"},\"tweet_id\":{"   \
    "\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"required\":[\"action\"]}"

typedef struct hu_twitter_ctx {
    char _unused;
} hu_twitter_ctx_t;

static hu_error_t twitter_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                  hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out)
        return HU_ERR_INVALID_ARGUMENT;
    const char *action = hu_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = hu_tool_result_fail("Missing 'action'", 16);
        return HU_OK;
    }

#if HU_IS_TEST
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "twitter: %s (test mode)", action);
    char *dup = hu_strndup(alloc, buf, (size_t)n);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out = hu_tool_result_ok_owned(dup, (size_t)n);
    return HU_OK;
#else
    (void)alloc;
    *out = hu_tool_result_fail("Twitter API not configured", 25);
    return HU_OK;
#endif
}

static void twitter_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_twitter_ctx_t));
}

static const char *twitter_name(void *ctx) {
    (void)ctx;
    return HU_TWITTER_NAME;
}

static const char *twitter_description(void *ctx) {
    (void)ctx;
    return HU_TWITTER_DESC;
}

static const char *twitter_parameters_json(void *ctx) {
    (void)ctx;
    return HU_TWITTER_PARAMS;
}

static const hu_tool_vtable_t twitter_vtable = {
    .name = twitter_name,
    .description = twitter_description,
    .parameters_json = twitter_parameters_json,
    .execute = twitter_execute,
    .deinit = twitter_deinit,
};

hu_error_t hu_twitter_tool_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_twitter_ctx_t *ctx = (hu_twitter_ctx_t *)alloc->alloc(alloc->ctx, sizeof(hu_twitter_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->_unused = 0;
    out->ctx = ctx;
    out->vtable = &twitter_vtable;
    return HU_OK;
}
