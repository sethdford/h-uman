#include "human/tools/agent_query.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#define TOOL_NAME "agent_query"
#define TOOL_DESC "Send a message to a running sub-agent and get its response."
#define TOOL_PARAMS                                                                \
    "{\"type\":\"object\",\"properties\":{"                                        \
    "\"agent_id\":{\"type\":\"integer\",\"description\":\"ID of the sub-agent\"}," \
    "\"message\":{\"type\":\"string\",\"description\":\"Message to send\"}"        \
    "},\"required\":[\"agent_id\",\"message\"]}"

typedef struct {
    hu_allocator_t *alloc;
    hu_agent_pool_t *pool;
} agent_query_ctx_t;

static hu_error_t agent_query_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    agent_query_ctx_t *c = (agent_query_ctx_t *)ctx;
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    double id_d = hu_json_get_number(args, "agent_id", -1.0);
    if (id_d < 0) {
        *out = hu_tool_result_fail("missing agent_id", 16);
        return HU_OK;
    }
    uint64_t agent_id = (uint64_t)id_d;
    const char *message = hu_json_get_string(args, "message");
    if (!message) {
        *out = hu_tool_result_fail("missing message", 15);
        return HU_OK;
    }

#if HU_IS_TEST
    (void)c;
    char *msg = hu_sprintf(alloc, "{\"agent_id\":%llu,\"response\":\"test response\"}",
                           (unsigned long long)agent_id);
    *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
    if (!c->pool) {
        *out = hu_tool_result_fail("agent pool not configured", 25);
        return HU_OK;
    }
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err =
        hu_agent_pool_query(c->pool, agent_id, message, strlen(message), &response, &response_len);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("query failed", 12);
        return HU_OK;
    }
    if (response) {
        *out = hu_tool_result_ok_owned(response, response_len);
    } else {
        *out = hu_tool_result_ok("{\"response\":null}", 17);
    }
#endif
    return HU_OK;
}

static const char *agent_query_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *agent_query_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *agent_query_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void agent_query_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(agent_query_ctx_t));
}

static const hu_tool_vtable_t agent_query_vtable = {
    .execute = agent_query_execute,
    .name = agent_query_name,
    .description = agent_query_desc,
    .parameters_json = agent_query_params,
    .deinit = agent_query_deinit,
};

hu_error_t hu_agent_query_tool_create(hu_allocator_t *alloc, hu_agent_pool_t *pool,
                                      hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    agent_query_ctx_t *c = (agent_query_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->pool = pool;
    out->ctx = c;
    out->vtable = &agent_query_vtable;
    return HU_OK;
}
