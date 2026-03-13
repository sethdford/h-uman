#include "human/tools/delegate.h"
#include "human/agent/registry.h"
#include "human/agent/spawn.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#ifdef HU_HAS_TOOLS_ADVANCED
#include "human/tools/claude_code.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_DELEGATE_NAME "delegate"
#define HU_DELEGATE_DESC                                                                        \
    "Delegate a task to a named sub-agent. Supported agents: 'claude_code' (coding via Claude " \
    "CLI)."
#define HU_DELEGATE_PARAMS                                                                  \
    "{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\",\"minLength\":1}," \
    "\"prompt\":{\"type\":\"string\",\"minLength\":1},\"context\":{\"type\":\"string\"},"   \
    "\"working_directory\":{\"type\":\"string\"}},\"required\":[\"agent\",\"prompt\"]}"

typedef struct hu_delegate_ctx {
    hu_agent_pool_t *pool;
    hu_agent_registry_t *registry;
#ifdef HU_HAS_TOOLS_ADVANCED
    hu_tool_t claude_code_tool;
    bool has_claude_code;
#endif
} hu_delegate_ctx_t;

static hu_error_t delegate_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *agent = hu_json_get_string(args, "agent");
    const char *prompt = hu_json_get_string(args, "prompt");
    if (!agent || strlen(agent) == 0) {
        *out = hu_tool_result_fail("missing agent", 13);
        return HU_OK;
    }
    if (!prompt || strlen(prompt) == 0) {
        *out = hu_tool_result_fail("missing prompt", 14);
        return HU_OK;
    }
#if HU_IS_TEST
    (void)ctx;
    size_t need = 25 + strlen(agent) + strlen(prompt);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "Delegated to agent '%s': %s", agent, prompt);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = hu_tool_result_ok_owned(msg, len);
    return HU_OK;
#else
    {
        hu_delegate_ctx_t *dctx = (hu_delegate_ctx_t *)ctx;
#ifdef HU_HAS_TOOLS_ADVANCED
        if (strcmp(agent, "claude_code") == 0 && dctx && dctx->has_claude_code) {
            return dctx->claude_code_tool.vtable->execute(dctx->claude_code_tool.ctx, alloc, args,
                                                          out);
        }
#endif
        if (dctx && dctx->registry) {
            const hu_named_agent_config_t *acfg =
                hu_agent_registry_get(dctx->registry, agent);
            if (acfg) {
                if (dctx->pool) {
                    uint64_t spawn_id = 0;
                    hu_error_t serr = hu_agent_pool_spawn_named(
                        dctx->pool, dctx->registry, agent, prompt, strlen(prompt), &spawn_id);
                    if (serr == HU_OK) {
                        char *msg = hu_sprintf(alloc,
                            "Delegated to agent '%s' (spawn_id=%llu): %s",
                            agent, (unsigned long long)spawn_id, prompt);
                        if (!msg) {
                            *out = hu_tool_result_fail("out of memory", 13);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                        *out = hu_tool_result_ok_owned(msg, strlen(msg));
                        return HU_OK;
                    }
                }
                const char *desc = acfg->description ? acfg->description : acfg->name;
                char *msg = hu_sprintf(alloc,
                    "Found agent '%s' (%s) but no pool available for spawning", agent, desc);
                if (!msg) {
                    *out = hu_tool_result_fail("out of memory", 13);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                *out = hu_tool_result_fail_owned(msg, strlen(msg));
                return HU_OK;
            }
        }

        size_t need = 48 + strlen(agent);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "Unknown agent '%s'. Available: (none)", agent);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = hu_tool_result_fail_owned(msg, len);
        return HU_OK;
    }
#endif
}

static const char *delegate_name(void *ctx) {
    (void)ctx;
    return HU_DELEGATE_NAME;
}
static const char *delegate_description(void *ctx) {
    (void)ctx;
    return HU_DELEGATE_DESC;
}
static const char *delegate_parameters_json(void *ctx) {
    (void)ctx;
    return HU_DELEGATE_PARAMS;
}
static void delegate_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx)
        return;
    hu_delegate_ctx_t *dctx = (hu_delegate_ctx_t *)ctx;
#ifdef HU_HAS_TOOLS_ADVANCED
    if (dctx->has_claude_code && dctx->claude_code_tool.vtable &&
        dctx->claude_code_tool.vtable->deinit) {
        dctx->claude_code_tool.vtable->deinit(dctx->claude_code_tool.ctx, alloc);
    }
#endif
    alloc->free(alloc->ctx, dctx, sizeof(*dctx));
}

static const hu_tool_vtable_t delegate_vtable = {
    .execute = delegate_execute,
    .name = delegate_name,
    .description = delegate_description,
    .parameters_json = delegate_parameters_json,
    .deinit = delegate_deinit,
};

hu_error_t hu_delegate_create(hu_allocator_t *alloc, hu_security_policy_t *policy,
                              hu_agent_pool_t *pool, hu_agent_registry_t *registry,
                              hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_delegate_ctx_t *c = (hu_delegate_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->pool = pool;
    c->registry = registry;

#ifdef HU_HAS_TOOLS_ADVANCED
    hu_error_t err = hu_claude_code_create(alloc, policy, &c->claude_code_tool);
    c->has_claude_code = (err == HU_OK);
#else
    (void)policy;
#endif

    out->ctx = c;
    out->vtable = &delegate_vtable;
    return HU_OK;
}
