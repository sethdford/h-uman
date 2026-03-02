#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tools/validation.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SC_FILE_WRITE_NAME "file_write"
#define SC_FILE_WRITE_DESC "Write content to file"
#define SC_FILE_WRITE_PARAMS "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}"

typedef struct sc_file_write_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_security_policy_t *policy;
} sc_file_write_ctx_t;

static sc_error_t file_write_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_file_write_ctx_t *c = (sc_file_write_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *path = sc_json_get_string(args, "path");
    const char *content = sc_json_get_string(args, "content");
    if (!path || strlen(path) == 0) {
        *out = sc_tool_result_fail("missing path", 12);
        return SC_OK;
    }
    if (!content) content = "";
    sc_error_t err = sc_tool_validate_path(path,
        c->workspace_dir, c->workspace_dir ? c->workspace_dir_len : 0);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("path traversal or invalid path", 30);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(file_write stub in test)", 25);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 25);
    return SC_OK;
#else
    char resolved[4096];
    const char *open_path = path;
    bool is_absolute = (path[0] == '/') ||
        (strlen(path) >= 2 && path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')));
    if (c->workspace_dir && c->workspace_dir_len > 0 && !is_absolute) {
        size_t n = c->workspace_dir_len;
        if (n >= sizeof(resolved) - 1) { *out = sc_tool_result_fail("path too long", 13); return SC_OK; }
        memcpy(resolved, c->workspace_dir, n);
        if (n > 0 && resolved[n - 1] != '/') { resolved[n] = '/'; n++; }
        size_t plen = strlen(path);
        if (n + plen >= sizeof(resolved)) { *out = sc_tool_result_fail("path too long", 13); return SC_OK; }
        memcpy(resolved + n, path, plen + 1);
        open_path = resolved;
    }
    if (c->policy && !sc_security_path_allowed(c->policy, open_path, strlen(open_path))) {
        *out = sc_tool_result_fail("path not allowed by policy", 26);
        return SC_OK;
    }
    size_t len = strlen(content);
    if (len > 1024 * 1024) {
        *out = sc_tool_result_fail("content too large", 16);
        return SC_OK;
    }
    FILE *f = fopen(open_path, "wb");
    if (!f) {
        *out = sc_tool_result_fail("failed to open file", 19);
        return SC_OK;
    }
    if (len > 0 && fwrite(content, 1, len, f) != len) {
        fclose(f);
        *out = sc_tool_result_fail("write failed", 11);
        return SC_OK;
    }
    fclose(f);
    char *ok = sc_strndup(alloc, "written", 7);
    if (!ok) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(ok, 7);
    return SC_OK;
#endif
}

static const char *file_write_name(void *ctx) { (void)ctx; return SC_FILE_WRITE_NAME; }
static const char *file_write_description(void *ctx) { (void)ctx; return SC_FILE_WRITE_DESC; }
static const char *file_write_parameters_json(void *ctx) { (void)ctx; return SC_FILE_WRITE_PARAMS; }
static void file_write_deinit(void *ctx, sc_allocator_t *alloc) {
    if (!ctx) return;
    sc_file_write_ctx_t *c = (sc_file_write_ctx_t *)ctx;
    if (c->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    free(c);
}

static const sc_tool_vtable_t file_write_vtable = {
    .execute = file_write_execute, .name = file_write_name,
    .description = file_write_description, .parameters_json = file_write_parameters_json,
    .deinit = file_write_deinit,
};

sc_error_t sc_file_write_create(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy,
    sc_tool_t *out)
{
    sc_file_write_ctx_t *c = (sc_file_write_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = sc_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &file_write_vtable;
    return SC_OK;
}
