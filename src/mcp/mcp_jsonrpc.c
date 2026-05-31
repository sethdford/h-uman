#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * JSON-RPC 2.0 protocol utilities for MCP.
 * Builds and parses JSON-RPC messages for remote MCP communication.
 */

/* ── JSON-RPC Request Builder ─────────────────────────────────────────── */

hu_error_t hu_mcp_jsonrpc_build_request(hu_allocator_t *alloc, uint32_t id, const char *method,
                                        const char *params_json, char **out, size_t *out_len) {
    if (!alloc || !method || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = HU_OK;

    /* Build: {"jsonrpc":"2.0","id":<id>,"method":"<method>","params":<params>} */
    err = hu_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    if (err != HU_OK)
        goto fail;

    char id_buf[32];
    int id_len = snprintf(id_buf, sizeof(id_buf), "%u", (unsigned)id);
    if (id_len < 0 || id_len >= (int)sizeof(id_buf)) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto fail;
    }
    err = hu_json_buf_append_raw(&buf, id_buf, (size_t)id_len);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, ",\"method\":\"", 11);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, method, strlen(method));
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "\",\"params\":", 11);
    if (err != HU_OK)
        goto fail;

    /* Use provided params or empty object */
    if (params_json && strlen(params_json) > 0) {
        err = hu_json_buf_append_raw(&buf, params_json, strlen(params_json));
    } else {
        err = hu_json_buf_append_raw(&buf, "{}", 2);
    }
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    *out = buf.ptr;
    *out_len = buf.len;
    return HU_OK;

fail:
    hu_json_buf_free(&buf);
    return err;
}

/* ── JSON-RPC Response Parser ────────────────────────────────────────── */

hu_error_t hu_mcp_jsonrpc_parse_response(hu_allocator_t *alloc, const char *json, size_t json_len,
                                         uint32_t *out_id, char **out_result,
                                         size_t *out_result_len, bool *out_is_error) {
    if (!alloc || !json || json_len == 0 || !out_id || !out_result || !out_result_len ||
        !out_is_error)
        return HU_ERR_INVALID_ARGUMENT;

    *out_id = 0;
    *out_result = NULL;
    *out_result_len = 0;
    *out_is_error = false;

    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &parsed);
    if (err != HU_OK)
        return err;

    if (!parsed || parsed->type != HU_JSON_OBJECT) {
        if (parsed)
            hu_json_free(alloc, parsed);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Extract "id" field */
    hu_json_value_t *id_val = hu_json_object_get(parsed, "id");
    if (id_val && id_val->type == HU_JSON_NUMBER) {
        *out_id = (uint32_t)id_val->data.number;
    }

    /* Check for error */
    hu_json_value_t *error_val = hu_json_object_get(parsed, "error");
    if (error_val && error_val->type != HU_JSON_NULL) {
        *out_is_error = true;
        /* Try to extract error message */
        hu_json_value_t *msg_val = hu_json_object_get(error_val, "message");
        if (msg_val && msg_val->type == HU_JSON_STRING) {
            *out_result =
                hu_strndup(alloc, msg_val->data.string.ptr, msg_val->data.string.len);
            if (*out_result)
                *out_result_len = msg_val->data.string.len;
        }
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    /* Extract "result" field */
    hu_json_value_t *result_val = hu_json_object_get(parsed, "result");
    if (!result_val) {
        hu_json_free(alloc, parsed);
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Stringify the result for returning */
    err = hu_json_stringify(alloc, result_val, out_result, out_result_len);
    hu_json_free(alloc, parsed);
    return err;
}

/* ── MCP-specific JSON-RPC Builders ────────────────────────────────── */

hu_error_t hu_mcp_jsonrpc_build_tools_list(hu_allocator_t *alloc, uint32_t id, char **out,
                                            size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* Build: {"jsonrpc":"2.0","id":<id>,"method":"tools/list","params":{}} */
    return hu_mcp_jsonrpc_build_request(alloc, id, "tools/list", "{}", out, out_len);
}

hu_error_t hu_mcp_jsonrpc_build_tools_call(hu_allocator_t *alloc, uint32_t id,
                                            const char *tool_name, const char *args_json,
                                            char **out, size_t *out_len) {
    if (!alloc || !tool_name || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t params_buf;
    if (hu_json_buf_init(&params_buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = HU_OK;

    /* Build params: {"name":"<tool_name>","arguments":<args>} */
    err = hu_json_buf_append_raw(&params_buf, "{\"name\":\"", 9);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&params_buf, tool_name, strlen(tool_name));
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&params_buf, "\",\"arguments\":", 14);
    if (err != HU_OK)
        goto fail;

    if (args_json && strlen(args_json) > 0) {
        err = hu_json_buf_append_raw(&params_buf, args_json, strlen(args_json));
    } else {
        err = hu_json_buf_append_raw(&params_buf, "{}", 2);
    }
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&params_buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    /* Now build the full request with these params */
    char *params_json = params_buf.ptr;

    err = hu_mcp_jsonrpc_build_request(alloc, id, "tools/call", params_json, out, out_len);
    hu_json_buf_free(&params_buf);
    return err;

fail:
    hu_json_buf_free(&params_buf);
    return err;
}
