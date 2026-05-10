#include "human/tools/agent_spawn.h"
#include "human/agent.h"
#include "human/agent/tool_context.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#define TOOL_NAME "agent_spawn"
#define TOOL_DESC \
    "Spawn a sub-agent to work on a task autonomously. Inherits parent tools, memory, SkillForge, " \
    "and policy when available. Returns an agent ID for querying."
#define TOOL_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{"                                                  \
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"}," \
    "\"label\":{\"type\":\"string\",\"description\":\"Short label for tracking\"},"          \
    "\"model\":{\"type\":\"string\",\"description\":\"Model override (optional)\"},"         \
    "\"mode\":{\"type\":\"string\",\"enum\":[\"one_shot\",\"persistent\"]}"                  \
    "},\"required\":[\"task\"]}"

typedef struct {
    hu_allocator_t *alloc;
    hu_agent_pool_t *pool;
} agent_spawn_ctx_t;

static hu_error_t agent_spawn_ok_json(hu_allocator_t *alloc, uint64_t agent_id, const char *label,
                                      hu_tool_result_t *out) {
    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    hu_json_value_t *idv = hu_json_number_new(alloc, (double)agent_id);
    if (!idv) {
        hu_json_free(alloc, root);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    if (hu_json_object_set(alloc, root, "agent_id", idv) != HU_OK) {
        hu_json_free(alloc, idv);
        hu_json_free(alloc, root);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    size_t label_len = label ? strlen(label) : 0;
    hu_json_value_t *labv = hu_json_string_new(alloc, label ? label : "", label_len);
    if (!labv) {
        hu_json_free(alloc, root);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    if (hu_json_object_set(alloc, root, "label", labv) != HU_OK) {
        hu_json_free(alloc, labv);
        hu_json_free(alloc, root);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    hu_json_value_t *statv = hu_json_string_new(alloc, "running", 7);
    if (!statv) {
        hu_json_free(alloc, root);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    if (hu_json_object_set(alloc, root, "status", statv) != HU_OK) {
        hu_json_free(alloc, statv);
        hu_json_free(alloc, root);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    char *msg = NULL;
    size_t msg_len = 0;
    hu_error_t jerr = hu_json_stringify(alloc, root, &msg, &msg_len);
    hu_json_free(alloc, root);
    if (jerr != HU_OK || !msg) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(msg, msg_len);
    return HU_OK;
}

static hu_error_t agent_spawn_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                      hu_tool_result_t *out) {
    agent_spawn_ctx_t *c = (agent_spawn_ctx_t *)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *task = hu_json_get_string(args, "task");
    if (!task) {
        *out = hu_tool_result_fail("missing task", 12);
        return HU_OK;
    }

    const char *label = hu_json_get_string(args, "label");
    if (!label)
        label = "sub-agent";

#if HU_IS_TEST
    (void)c;
    return agent_spawn_ok_json(alloc, 1u, label, out);
#else
    if (!c) {
        *out = hu_tool_result_fail("agent spawn not configured", 27);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!c->pool) {
        *out = hu_tool_result_fail("agent pool not configured", 25);
        return HU_OK;
    }
    hu_spawn_config_t cfg = {0};
    const char *mode_str = hu_json_get_string(args, "mode");
    if (mode_str && strcmp(mode_str, "persistent") == 0)
        cfg.mode = HU_SPAWN_PERSISTENT;
    else
        cfg.mode = HU_SPAWN_ONE_SHOT;

    hu_agent_t *parent = hu_agent_get_current_for_tools();
    if (parent && parent->persona_name && parent->persona_name_len > 0) {
        cfg.persona_name = parent->persona_name;
        cfg.persona_name_len = parent->persona_name_len;
    }
#ifdef HU_HAS_SKILLS
    if (parent && parent->skillforge)
        cfg.skillforge = parent->skillforge;
#endif
    if (parent && parent->tools && parent->tools_count > 0) {
        cfg.parent_tools = parent->tools;
        cfg.parent_tools_count = parent->tools_count;
    }
    if (parent && parent->memory)
        cfg.memory = parent->memory;
    if (parent && parent->verifier_graph)
        cfg.verifier_graph = parent->verifier_graph;
    if (parent && parent->session_store)
        cfg.session_store = parent->session_store;
    if (parent && parent->observer && parent->observer->vtable)
        cfg.observer = parent->observer;
    if (parent && parent->policy)
        cfg.policy = parent->policy;
    if (parent)
        cfg.autonomy_level = parent->autonomy_level;
    if (parent) {
        cfg.caller_spawn_depth = parent->spawn_depth;
        cfg.shared_cost_tracker = parent->cost_tracker;
        cfg.metacognition_policy = &parent->infra.metacognition.cfg;
    }

    const char *model = hu_json_get_string(args, "model");
    if (model) {
        cfg.model = model;
        cfg.model_len = strlen(model);
    }

    uint64_t agent_id = 0;
    hu_error_t err = hu_agent_pool_spawn(c->pool, &cfg, task, strlen(task), label, &agent_id);
    if (err != HU_OK) {
        hu_log_error("agent_spawn", NULL, "spawn failed: %s", hu_error_string(err));
        static const char fail_msg[] = "agent spawn failed";
        *out = hu_tool_result_fail(fail_msg, sizeof(fail_msg) - 1);
        return HU_OK;
    }
    return agent_spawn_ok_json(alloc, agent_id, label, out);
#endif
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
static void agent_spawn_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(agent_spawn_ctx_t));
}

static const hu_tool_vtable_t agent_spawn_vtable = {
    .execute = agent_spawn_execute,
    .name = agent_spawn_name,
    .description = agent_spawn_desc,
    .parameters_json = agent_spawn_params,
    .deinit = agent_spawn_deinit,
};

hu_error_t hu_agent_spawn_tool_create(hu_allocator_t *alloc, hu_agent_pool_t *pool,
                                      hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    agent_spawn_ctx_t *c = (agent_spawn_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->pool = pool;
    out->ctx = c;
    out->vtable = &agent_spawn_vtable;
    return HU_OK;
}
