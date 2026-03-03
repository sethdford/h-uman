#include "seaclaw/tools/agent_spawn.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>

#define TOOL_NAME "agent_spawn"
#define TOOL_DESC \
    "Spawn a sub-agent to work on a task autonomously. Returns an agent ID for querying."
#define TOOL_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{"                                                  \
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"}," \
    "\"label\":{\"type\":\"string\",\"description\":\"Short label for tracking\"},"          \
    "\"model\":{\"type\":\"string\",\"description\":\"Model override (optional)\"},"         \
    "\"mode\":{\"type\":\"string\",\"enum\":[\"one_shot\",\"persistent\"]}"                  \
    "},\"required\":[\"task\"]}"

typedef struct {
    sc_allocator_t *alloc;
    sc_agent_pool_t *pool;
} agent_spawn_ctx_t;

static sc_error_t agent_spawn_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                      sc_tool_result_t *out) {
    agent_spawn_ctx_t *c = (agent_spawn_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }

    const char *task = sc_json_get_string(args, "task");
    if (!task) {
        *out = sc_tool_result_fail("missing task", 12);
        return SC_OK;
    }

    const char *label = sc_json_get_string(args, "label");
    if (!label)
        label = "sub-agent";

#if SC_IS_TEST
    (void)c;
    char *msg =
        sc_sprintf(alloc, "{\"agent_id\":1,\"label\":\"%s\",\"status\":\"running\"}", label);
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#else
    if (!c->pool) {
        *out = sc_tool_result_fail("agent pool not configured", 25);
        return SC_OK;
    }
    sc_spawn_config_t cfg = {0};
    const char *mode_str = sc_json_get_string(args, "mode");
    if (mode_str && strcmp(mode_str, "persistent") == 0)
        cfg.mode = SC_SPAWN_PERSISTENT;
    else
        cfg.mode = SC_SPAWN_ONE_SHOT;

    const char *model = sc_json_get_string(args, "model");
    if (model) {
        cfg.model = model;
        cfg.model_len = strlen(model);
    }

    uint64_t agent_id = 0;
    sc_error_t err = sc_agent_pool_spawn(c->pool, &cfg, task, strlen(task), label, &agent_id);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("spawn failed", 12);
        return SC_OK;
    }
    char *msg = sc_sprintf(alloc, "{\"agent_id\":%llu,\"label\":\"%s\",\"status\":\"running\"}",
                           (unsigned long long)agent_id, label);
    *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
#endif
    return SC_OK;
}

static const char *agent_spawn_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *agent_spawn_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *agent_spawn_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void agent_spawn_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(agent_spawn_ctx_t));
}

static const sc_tool_vtable_t agent_spawn_vtable = {
    .execute = agent_spawn_execute,
    .name = agent_spawn_name,
    .description = agent_spawn_desc,
    .parameters_json = agent_spawn_params,
    .deinit = agent_spawn_deinit,
};

sc_error_t sc_agent_spawn_tool_create(sc_allocator_t *alloc, sc_agent_pool_t *pool,
                                      sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    agent_spawn_ctx_t *c = (agent_spawn_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->pool = pool;
    out->ctx = c;
    out->vtable = &agent_spawn_vtable;
    return SC_OK;
}
