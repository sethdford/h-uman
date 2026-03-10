#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "human/tools/schema_common.h"
#define HU_FILE_MAX_SIZE     (1024 * 1024)
#define HU_FILE_WRITE_NAME   "file_write"
#define HU_FILE_WRITE_DESC   "Write content to file"
#define HU_FILE_WRITE_PARAMS HU_SCHEMA_PATH_CONTENT

typedef struct hu_file_write_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_security_policy_t *policy;
} hu_file_write_ctx_t;

static hu_error_t file_write_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                     hu_tool_result_t *out) {
    hu_file_write_ctx_t *c = (hu_file_write_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *path = hu_json_get_string(args, "path");
    const char *content = hu_json_get_string(args, "content");
    if (!path || strlen(path) == 0) {
        *out = hu_tool_result_fail("missing path", 12);
        return HU_OK;
    }
    if (!content)
        content = "";
    hu_error_t err =
        hu_tool_validate_path(path, c->workspace_dir, c->workspace_dir ? c->workspace_dir_len : 0);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("path traversal or invalid path", 30);
        return HU_OK;
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(file_write stub in test)", 25);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 25);
    return HU_OK;
#else
    char resolved[4096];
    const char *open_path = path;
    bool is_absolute = (path[0] == '/') ||
                       (strlen(path) >= 2 && path[1] == ':' &&
                        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')));
    if (c->workspace_dir && c->workspace_dir_len > 0 && !is_absolute) {
        size_t n = c->workspace_dir_len;
        if (n >= sizeof(resolved) - 1) {
            *out = hu_tool_result_fail("path too long", 13);
            return HU_OK;
        }
        memcpy(resolved, c->workspace_dir, n);
        if (n > 0 && resolved[n - 1] != '/') {
            resolved[n] = '/';
            n++;
        }
        size_t plen = strlen(path);
        if (n + plen >= sizeof(resolved)) {
            *out = hu_tool_result_fail("path too long", 13);
            return HU_OK;
        }
        memcpy(resolved + n, path, plen + 1);
        open_path = resolved;
    }
    if (!c->policy || !hu_security_path_allowed(c->policy, open_path, strlen(open_path))) {
        *out = hu_tool_result_fail("path not allowed by policy", 26);
        return HU_OK;
    }
    size_t len = strlen(content);
    if (len > HU_FILE_MAX_SIZE) {
        *out = hu_tool_result_fail("content too large", 16);
        return HU_OK;
    }
    FILE *f = fopen(open_path, "wb");
    if (!f) {
        *out = hu_tool_result_fail("failed to open file", 19);
        return HU_OK;
    }
    if (len > 0 && fwrite(content, 1, len, f) != len) {
        fclose(f);
        *out = hu_tool_result_fail("write failed", 11);
        return HU_OK;
    }
    fclose(f);
    char *ok = hu_strndup(alloc, "written", 7);
    if (!ok) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(ok, 7);
    return HU_OK;
#endif
}

static const char *file_write_name(void *ctx) {
    (void)ctx;
    return HU_FILE_WRITE_NAME;
}
static const char *file_write_description(void *ctx) {
    (void)ctx;
    return HU_FILE_WRITE_DESC;
}
static const char *file_write_parameters_json(void *ctx) {
    (void)ctx;
    return HU_FILE_WRITE_PARAMS;
}
static void file_write_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx)
        return;
    hu_file_write_ctx_t *c = (hu_file_write_ctx_t *)ctx;
    if (c->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    if (alloc)
        alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t file_write_vtable = {
    .execute = file_write_execute,
    .name = file_write_name,
    .description = file_write_description,
    .parameters_json = file_write_parameters_json,
    .deinit = file_write_deinit,
};

hu_error_t hu_file_write_create(hu_allocator_t *alloc, const char *workspace_dir,
                                size_t workspace_dir_len, hu_security_policy_t *policy,
                                hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_file_write_ctx_t *c = (hu_file_write_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = hu_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &file_write_vtable;
    return HU_OK;
}
