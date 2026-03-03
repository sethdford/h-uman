/*
 * Template: Custom Tool
 *
 * Implements sc_tool_t vtable. Required methods:
 *   - execute: run with JSON args, write result to out
 *   - name: return tool name (static string)
 *   - description: return description for LLM (static string)
 *   - parameters_json: JSON Schema for args (static string)
 *
 * Optional: deinit for heap-allocated context cleanup
 */
#include "my_tool.h"
#include "seaclaw/tool.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>

#define SC_MY_TOOL_NAME "my_tool"
#define SC_MY_TOOL_DESC "Does something useful. Describe what the tool does for the LLM."
#define SC_MY_TOOL_PARAMS "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}},\"required\":[\"input\"]}"

typedef struct sc_my_tool_ctx {
    /* Add per-instance state if needed */
} sc_my_tool_ctx_t;

static sc_error_t my_tool_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_my_tool_ctx_t *c = (sc_my_tool_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }

    const char *input = sc_json_get_string(args, "input");
    if (!input || strlen(input) == 0) {
        *out = sc_tool_result_fail("Missing required 'input' parameter", 31);
        return SC_OK;
    }

#if SC_IS_TEST
    /* In tests: no side effects (network, spawn, etc.) */
    char *msg = sc_strndup(alloc, "(my_tool stub)", 14);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 14);
    return SC_OK;
#else
    /* TODO: Implement real logic. Validate inputs, perform action.
     * Use sc_tool_result_ok / sc_tool_result_ok_owned for success.
     * Use sc_tool_result_fail / sc_tool_result_fail_owned for errors.
     * Caller must sc_tool_result_free if output_owned or error_msg_owned. */
    (void)alloc;
    (void)input;
    *out = sc_tool_result_fail("Not implemented", 14);
    return SC_OK;
#endif
}

static const char *my_tool_name(void *ctx) {
    (void)ctx;
    return SC_MY_TOOL_NAME;
}

static const char *my_tool_description(void *ctx) {
    (void)ctx;
    return SC_MY_TOOL_DESC;
}

static const char *my_tool_parameters_json(void *ctx) {
    (void)ctx;
    return SC_MY_TOOL_PARAMS;
}

static void my_tool_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    if (ctx) free(ctx);
}

static const sc_tool_vtable_t my_tool_vtable = {
    .execute = my_tool_execute,
    .name = my_tool_name,
    .description = my_tool_description,
    .parameters_json = my_tool_parameters_json,
    .deinit = my_tool_deinit,
};

sc_error_t sc_my_tool_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;

    sc_my_tool_ctx_t *ctx =
        (sc_my_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));

    out->ctx = ctx;
    out->vtable = &my_tool_vtable;
    return SC_OK;
}
