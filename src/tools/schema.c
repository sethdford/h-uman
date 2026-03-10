#include "human/tools/schema.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/schema_clean.h"
#include <stdlib.h>
#include <string.h>

#define SCHEMA_PARAMS                                                                              \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"validate\"," \
    "\"list\",\"clean\"]},\"schema\":{\"type\":\"string\"},\"provider\":{\"type\":\"string\","     \
    "\"enum\":[\"gemini\",\"anthropic\",\"openai\",\"conservative\"]}},\"required\":[\"action\"]}"

static hu_error_t schema_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                 hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = hu_tool_result_fail("Missing 'action' parameter", 27);
        return HU_OK;
    }
    if (strcmp(action, "validate") == 0) {
        const char *schema = hu_json_get_string(args, "schema");
        if (!schema || !schema[0]) {
            *out = hu_tool_result_fail("Missing 'schema' for validate action", 37);
            return HU_OK;
        }
        bool valid = hu_schema_validate(alloc, schema, strlen(schema));
        char *msg =
            hu_strndup(alloc, valid ? "Schema is valid" : "Invalid JSON schema", valid ? 14 : 19);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, valid ? 14 : 19);
        return HU_OK;
    }
    if (strcmp(action, "clean") == 0) {
        const char *schema = hu_json_get_string(args, "schema");
        const char *provider = hu_json_get_string(args, "provider");
        if (!schema || !schema[0]) {
            *out = hu_tool_result_fail("Missing 'schema' for clean action", 33);
            return HU_OK;
        }
        if (!provider || !provider[0])
            provider = "conservative";
        char *cleaned = NULL;
        size_t cleaned_len = 0;
        hu_error_t err =
            hu_schema_clean(alloc, schema, strlen(schema), provider, &cleaned, &cleaned_len);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Schema clean failed", 18);
            return HU_OK;
        }
        *out = hu_tool_result_ok_owned(cleaned, cleaned_len);
        return HU_OK;
    }
    if (strcmp(action, "list") == 0) {
        char *msg = hu_strndup(
            alloc, "[\"shell\",\"file_read\",\"cron_add\",\"http_request\",\"memory_store\"]", 54);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 54);
        return HU_OK;
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
}

static const char *schema_name(void *ctx) {
    (void)ctx;
    return "schema";
}
static const char *schema_desc(void *ctx) {
    (void)ctx;
    return "Validate JSON schemas or list available tool schemas.";
}
static const char *schema_params(void *ctx) {
    (void)ctx;
    return SCHEMA_PARAMS;
}
static void schema_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, 1);
}

static const hu_tool_vtable_t schema_vtable = {
    .execute = schema_execute,
    .name = schema_name,
    .description = schema_desc,
    .parameters_json = schema_params,
    .deinit = schema_deinit,
};

hu_error_t hu_schema_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, 1);
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, 1);
    out->ctx = ctx;
    out->vtable = &schema_vtable;
    return HU_OK;
}
