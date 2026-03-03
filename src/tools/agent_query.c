#include "seaclaw/tools/agent_query.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
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
    sc_allocator_t *alloc;
    sc_agent_pool_t *pool;
} agent_query_ctx_t;

static sc_error_t agent_query_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                      sc_tool_result_t *out) {
    agent_query_ctx_t *c = (agent_query_ctx_t *)ctx;
    (void)alloc;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }

    double id_d = sc_json_get_number(args, "agent_id", -1.0);
    if (id_d < 0) {
        *out = sc_tool_result_fail("missing agent_id", 16);
        return SC_OK;
    }
    uint64_t agent_id = (uint64_t)id_d;
    const char *message = sc_json_get_string(args, "message");
    if (!message) {
        *out = sc_tool_result_fail("missing message", 15);
        return SC_OK;
    }

#if SC_IS_TEST
    (void)c;
    char *msg = sc_sprintf(alloc, "{\"agent_id\":%llu,\"response\":\"test response\"}",
                           (unsigned long long)agent_id);
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
    if (!c->pool) {
        *out = sc_tool_result_fail("agent pool not configured", 25);
        return SC_OK;
    }
    char *response = NULL;
    size_t response_len = 0;
    sc_error_t err =
        sc_agent_pool_query(c->pool, agent_id, message, strlen(message), &response, &response_len);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("query failed", 12);
        return SC_OK;
    }
    if (response) {
        *out = sc_tool_result_ok_owned(response, response_len);
    } else {
        *out = sc_tool_result_ok("{\"response\":null}", 17);
    }
#endif
    return SC_OK;
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
static void agent_query_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(agent_query_ctx_t));
}

static const sc_tool_vtable_t agent_query_vtable = {
    .execute = agent_query_execute,
    .name = agent_query_name,
    .description = agent_query_desc,
    .parameters_json = agent_query_params,
    .deinit = agent_query_deinit,
};

sc_error_t sc_agent_query_tool_create(sc_allocator_t *alloc, sc_agent_pool_t *pool,
                                      sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    agent_query_ctx_t *c = (agent_query_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->pool = pool;
    out->ctx = c;
    out->vtable = &agent_query_vtable;
    return SC_OK;
}
