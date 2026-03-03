#include "seaclaw/tools/delegate.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#ifdef SC_HAS_TOOLS_ADVANCED
#include "seaclaw/tools/claude_code.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_DELEGATE_NAME "delegate"
#define SC_DELEGATE_DESC                                                                        \
    "Delegate a task to a named sub-agent. Supported agents: 'claude_code' (coding via Claude " \
    "CLI)."
#define SC_DELEGATE_PARAMS                                                                  \
    "{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\",\"minLength\":1}," \
    "\"prompt\":{\"type\":\"string\",\"minLength\":1},\"context\":{\"type\":\"string\"},"   \
    "\"working_directory\":{\"type\":\"string\"}},\"required\":[\"agent\",\"prompt\"]}"

typedef struct sc_delegate_ctx {
#ifdef SC_HAS_TOOLS_ADVANCED
    sc_tool_t claude_code_tool;
    bool has_claude_code;
#else
    char _pad;
#endif
} sc_delegate_ctx_t;

static sc_error_t delegate_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                   sc_tool_result_t *out) {
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *agent = sc_json_get_string(args, "agent");
    const char *prompt = sc_json_get_string(args, "prompt");
    if (!agent || strlen(agent) == 0) {
        *out = sc_tool_result_fail("missing agent", 13);
        return SC_OK;
    }
    if (!prompt || strlen(prompt) == 0) {
        *out = sc_tool_result_fail("missing prompt", 14);
        return SC_OK;
    }
#if SC_IS_TEST
    (void)ctx;
    size_t need = 25 + strlen(agent) + strlen(prompt);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "Delegated to agent '%s': %s", agent, prompt);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#else
#ifdef SC_HAS_TOOLS_ADVANCED
    {
        sc_delegate_ctx_t *dctx = (sc_delegate_ctx_t *)ctx;
        if (strcmp(agent, "claude_code") == 0 && dctx && dctx->has_claude_code) {
            return dctx->claude_code_tool.vtable->execute(dctx->claude_code_tool.ctx, alloc, args,
                                                          out);
        }
    }
#else
    (void)ctx;
#endif

    size_t need = 48 + strlen(agent);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 13);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, need + 1, "Unknown agent '%s'. Available: (none)", agent);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_fail_owned(msg, len);
    return SC_OK;
#endif
}

static const char *delegate_name(void *ctx) {
    (void)ctx;
    return SC_DELEGATE_NAME;
}
static const char *delegate_description(void *ctx) {
    (void)ctx;
    return SC_DELEGATE_DESC;
}
static const char *delegate_parameters_json(void *ctx) {
    (void)ctx;
    return SC_DELEGATE_PARAMS;
}
static void delegate_deinit(void *ctx, sc_allocator_t *alloc) {
    if (!ctx)
        return;
    sc_delegate_ctx_t *dctx = (sc_delegate_ctx_t *)ctx;
#ifdef SC_HAS_TOOLS_ADVANCED
    if (dctx->has_claude_code && dctx->claude_code_tool.vtable &&
        dctx->claude_code_tool.vtable->deinit) {
        dctx->claude_code_tool.vtable->deinit(dctx->claude_code_tool.ctx, alloc);
    }
#endif
    alloc->free(alloc->ctx, dctx, sizeof(*dctx));
}

static const sc_tool_vtable_t delegate_vtable = {
    .execute = delegate_execute,
    .name = delegate_name,
    .description = delegate_description,
    .parameters_json = delegate_parameters_json,
    .deinit = delegate_deinit,
};

sc_error_t sc_delegate_create(sc_allocator_t *alloc, sc_security_policy_t *policy, sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_delegate_ctx_t *c = (sc_delegate_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));

#ifdef SC_HAS_TOOLS_ADVANCED
    sc_error_t err = sc_claude_code_create(alloc, policy, &c->claude_code_tool);
    c->has_claude_code = (err == SC_OK);
#else
    (void)policy;
#endif

    out->ctx = c;
    out->vtable = &delegate_vtable;
    return SC_OK;
}
