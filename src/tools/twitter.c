/*
 * Twitter/X tool — post tweets, search, reply.
 */
#include "seaclaw/tools/twitter.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <string.h>

#define SC_TWITTER_NAME "twitter"
#define SC_TWITTER_DESC "Post tweets, search Twitter/X, reply to tweets"
#define SC_TWITTER_PARAMS                                                             \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[" \
    "\"post\",\"search\",\"reply\"]},\"text\":{\"type\":\"string\"},\"tweet_id\":{"   \
    "\"type\":\"string\"},\"query\":{\"type\":\"string\"}},\"required\":[\"action\"]}"

typedef struct sc_twitter_ctx {
    char _unused;
} sc_twitter_ctx_t;

static sc_error_t twitter_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                  sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out)
        return SC_ERR_INVALID_ARGUMENT;
    const char *action = sc_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = sc_tool_result_fail("Missing 'action'", 16);
        return SC_OK;
    }

#if SC_IS_TEST
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "twitter: %s (test mode)", action);
    char *dup = sc_strndup(alloc, buf, (size_t)n);
    if (!dup)
        return SC_ERR_OUT_OF_MEMORY;
    *out = sc_tool_result_ok_owned(dup, (size_t)n);
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_fail("Twitter API not configured", 25);
    return SC_OK;
#endif
}

static void twitter_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(sc_twitter_ctx_t));
}

static const char *twitter_name(void *ctx) {
    (void)ctx;
    return SC_TWITTER_NAME;
}

static const char *twitter_description(void *ctx) {
    (void)ctx;
    return SC_TWITTER_DESC;
}

static const char *twitter_parameters_json(void *ctx) {
    (void)ctx;
    return SC_TWITTER_PARAMS;
}

static const sc_tool_vtable_t twitter_vtable = {
    .name = twitter_name,
    .description = twitter_description,
    .parameters_json = twitter_parameters_json,
    .execute = twitter_execute,
    .deinit = twitter_deinit,
};

sc_error_t sc_twitter_tool_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_twitter_ctx_t *ctx = (sc_twitter_ctx_t *)alloc->alloc(alloc->ctx, sizeof(sc_twitter_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    ctx->_unused = 0;
    out->ctx = ctx;
    out->vtable = &twitter_vtable;
    return SC_OK;
}
