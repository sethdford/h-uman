#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_DELEGATE_NAME "delegate"
#define SC_DELEGATE_DESC "Delegate a task to a named sub-agent."
#define SC_DELEGATE_PARAMS "{\"type\":\"object\",\"properties\":{\"agent\":{\"type\":\"string\",\"minLength\":1},\"prompt\":{\"type\":\"string\",\"minLength\":1},\"context\":{\"type\":\"string\"}},\"required\":[\"agent\",\"prompt\"]}"

typedef struct sc_delegate_ctx {
    void *placeholder;
} sc_delegate_ctx_t;

static sc_error_t delegate_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *agent = sc_json_get_string(args, "agent");
    const char *prompt = sc_json_get_string(args, "prompt");
    const char *context = sc_json_get_string(args, "context");
    (void)context;
    if (!agent || strlen(agent) == 0) {
        *out = sc_tool_result_fail("missing agent", 13);
        return SC_OK;
    }
    if (!prompt || strlen(prompt) == 0) {
        *out = sc_tool_result_fail("missing prompt", 14);
        return SC_OK;
    }
#if SC_IS_TEST
    size_t need = 25 + strlen(agent) + strlen(prompt);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    int n = snprintf(msg, need + 1, "Delegated to agent '%s': %s", agent, prompt);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_fail("Delegate requires subagent runtime", 34);
    return SC_OK;
#endif
}

static const char *delegate_name(void *ctx) { (void)ctx; return SC_DELEGATE_NAME; }
static const char *delegate_description(void *ctx) { (void)ctx; return SC_DELEGATE_DESC; }
static const char *delegate_parameters_json(void *ctx) { (void)ctx; return SC_DELEGATE_PARAMS; }
static void delegate_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t delegate_vtable = {
    .execute = delegate_execute, .name = delegate_name,
    .description = delegate_description, .parameters_json = delegate_parameters_json,
    .deinit = delegate_deinit,
};

sc_error_t sc_delegate_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    sc_delegate_ctx_t *c = (sc_delegate_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = c;
    out->vtable = &delegate_vtable;
    return SC_OK;
}
