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

#define SC_FILE_READ_NAME "file_read"
#define SC_FILE_READ_DESC "Read file contents from path"
#define SC_FILE_READ_PARAMS "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"

typedef struct sc_file_read_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_security_policy_t *policy;
} sc_file_read_ctx_t;

static sc_error_t file_read_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_file_read_ctx_t *c = (sc_file_read_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *path = sc_json_get_string(args, "path");
    if (!path || strlen(path) == 0) {
        *out = sc_tool_result_fail("missing path", 12);
        return SC_OK;
    }
    sc_error_t err = sc_tool_validate_path(path,
        c->workspace_dir, c->workspace_dir ? c->workspace_dir_len : 0);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("path traversal or invalid path", 30);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(file_read stub in test)", 24);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 24);
    return SC_OK;
#else
    /* Resolve relative path against workspace */
    char resolved[4096];
    const char *open_path = path;
    /* Relative path: resolve against workspace (not / or C:\) */
    bool is_absolute = (path[0] == '/') ||
        (strlen(path) >= 2 && path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')));
    if (c->workspace_dir && c->workspace_dir_len > 0 && !is_absolute) {
        size_t n = c->workspace_dir_len;
        if (n >= sizeof(resolved) - 1) {
            *out = sc_tool_result_fail("path too long", 13);
            return SC_OK;
        }
        memcpy(resolved, c->workspace_dir, n);
        if (n > 0 && resolved[n - 1] != '/') { resolved[n] = '/'; n++; }
        size_t plen = strlen(path);
        if (n + plen >= sizeof(resolved)) {
            *out = sc_tool_result_fail("path too long", 13);
            return SC_OK;
        }
        memcpy(resolved + n, path, plen + 1);
        open_path = resolved;
    }
    if (c->policy && !sc_security_path_allowed(c->policy, open_path, strlen(open_path))) {
        *out = sc_tool_result_fail("path not allowed by policy", 26);
        return SC_OK;
    }
    FILE *f = fopen(open_path, "rb");
    if (!f) {
        *out = sc_tool_result_fail("failed to open file", 19);
        return SC_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        *out = sc_tool_result_fail("file too large or empty", 23);
        return SC_OK;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) { fclose(f); *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out = sc_tool_result_ok_owned(buf, n);
    return SC_OK;
#endif
}

static const char *file_read_name(void *ctx) { (void)ctx; return SC_FILE_READ_NAME; }
static const char *file_read_description(void *ctx) { (void)ctx; return SC_FILE_READ_DESC; }
static const char *file_read_parameters_json(void *ctx) { (void)ctx; return SC_FILE_READ_PARAMS; }
static void file_read_deinit(void *ctx, sc_allocator_t *alloc) {
    if (!ctx) return;
    sc_file_read_ctx_t *c = (sc_file_read_ctx_t *)ctx;
    if (c->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    free(c);
}

static const sc_tool_vtable_t file_read_vtable = {
    .execute = file_read_execute, .name = file_read_name,
    .description = file_read_description, .parameters_json = file_read_parameters_json,
    .deinit = file_read_deinit,
};

sc_error_t sc_file_read_create(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy,
    sc_tool_t *out)
{
    sc_file_read_ctx_t *c = (sc_file_read_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = sc_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &file_read_vtable;
    return SC_OK;
}
