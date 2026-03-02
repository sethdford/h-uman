#include "seaclaw/tool.h"
#include "seaclaw/tools/schema.h"
#include "seaclaw/tools/schema_clean.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

#define SCHEMA_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"validate\",\"list\",\"clean\"]},\"schema\":{\"type\":\"string\"},\"provider\":{\"type\":\"string\",\"enum\":[\"gemini\",\"anthropic\",\"openai\",\"conservative\"]}},\"required\":[\"action\"]}"

static sc_error_t schema_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'action' parameter", 27);
        return SC_OK;
    }
    if (strcmp(action, "validate") == 0) {
        const char *schema = sc_json_get_string(args, "schema");
        if (!schema || !schema[0]) {
            *out = sc_tool_result_fail("Missing 'schema' for validate action", 37);
            return SC_OK;
        }
        bool valid = sc_schema_validate(alloc, schema, strlen(schema));
        char *msg = sc_strndup(alloc, valid ? "Schema is valid" : "Invalid JSON schema",
            valid ? 14 : 19);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, valid ? 14 : 19);
        return SC_OK;
    }
    if (strcmp(action, "clean") == 0) {
        const char *schema = sc_json_get_string(args, "schema");
        const char *provider = sc_json_get_string(args, "provider");
        if (!schema || !schema[0]) {
            *out = sc_tool_result_fail("Missing 'schema' for clean action", 33);
            return SC_OK;
        }
        if (!provider || !provider[0]) provider = "conservative";
        char *cleaned = NULL;
        size_t cleaned_len = 0;
        sc_error_t err = sc_schema_clean(alloc, schema, strlen(schema), provider, &cleaned, &cleaned_len);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Schema clean failed", 18);
            return SC_OK;
        }
        *out = sc_tool_result_ok_owned(cleaned, cleaned_len);
        return SC_OK;
    }
    if (strcmp(action, "list") == 0) {
        char *msg = sc_strndup(alloc, "[\"shell\",\"file_read\",\"cron_add\",\"http_request\",\"memory_store\"]", 54);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 54);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
}

static const char *schema_name(void *ctx) { (void)ctx; return "schema"; }
static const char *schema_desc(void *ctx) {
    (void)ctx;
    return "Validate JSON schemas or list available tool schemas.";
}
static const char *schema_params(void *ctx) { (void)ctx; return SCHEMA_PARAMS; }
static void schema_deinit(void *ctx, sc_allocator_t *alloc) { (void)ctx; (void)alloc; free(ctx); }

static const sc_tool_vtable_t schema_vtable = {
    .execute = schema_execute, .name = schema_name,
    .description = schema_desc, .parameters_json = schema_params,
    .deinit = schema_deinit,
};

sc_error_t sc_schema_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    void *ctx = calloc(1, 1);
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &schema_vtable;
    return SC_OK;
}
