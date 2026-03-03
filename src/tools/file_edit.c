/*
 * file_edit tool: Find and replace text in a file.
 * Find and replace text in a file.
 * - Atomic writes (temp file + rename)
 * - Symlink detection via realpath
 * - Path security: sc_path_is_safe, sc_path_resolved_allowed
 * - Pattern matching: replace first occurrence of old_text with new_text
 */
#include "seaclaw/tools/file_edit.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/path_security.h"
#include "seaclaw/tools/validation.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define SC_FILE_EDIT_NAME "file_edit"
#define SC_FILE_EDIT_DESC "Find and replace text in a file"
#define SC_FILE_EDIT_PARAMS                                                                       \
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":" \
    "\"string\"},\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_text\",\"new_"  \
    "text\"]}"
#define SC_FILE_EDIT_MAX_SIZE (10 * 1024 * 1024) /* 10 MB */
#define SC_PATH_BUF           4096

typedef struct sc_file_edit_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_security_policy_t *policy;
} sc_file_edit_ctx_t;

static sc_error_t file_edit_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                    sc_tool_result_t *out) {
    sc_file_edit_ctx_t *c = (sc_file_edit_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }

    const char *path = sc_json_get_string(args, "path");
    const char *old_text = sc_json_get_string(args, "old_text");
    const char *new_text = sc_json_get_string(args, "new_text");

    if (!path || path[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'path' parameter", 22);
        return SC_OK;
    }
    if (!old_text) {
        *out = sc_tool_result_fail("Missing 'old_text' parameter", 26);
        return SC_OK;
    }
    if (!new_text) {
        *out = sc_tool_result_fail("Missing 'new_text' parameter", 26);
        return SC_OK;
    }
    size_t old_len = strlen(old_text);
    if (old_len == 0) {
        *out = sc_tool_result_fail("old_text must not be empty", 26);
        return SC_OK;
    }

#if SC_IS_TEST
    /* In test mode, stub returns success to avoid filesystem side effects */
    char *msg = sc_strndup(alloc, "(file_edit stub in test)", 24);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 24);
    return SC_OK;
#else
    bool is_absolute =
        (path[0] == '/') ||
        (strlen(path) >= 3 &&
         (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && (path[2] == '/' || path[2] == '\\')));
#ifdef _WIN32
    is_absolute = is_absolute || (strlen(path) >= 2 && path[0] == '\\' && path[1] == '\\');
#endif

    char full_path_buf[SC_PATH_BUF];
    const char *full_path = path;
    if (!is_absolute) {
        if (!sc_path_is_safe(path)) {
            *out = sc_tool_result_fail("Path not allowed: contains traversal or absolute path", 51);
            return SC_OK;
        }
        if (!c->workspace_dir || c->workspace_dir_len == 0) {
            *out = sc_tool_result_fail("Workspace not configured", 24);
            return SC_OK;
        }
        size_t n = c->workspace_dir_len;
        if (n >= SC_PATH_BUF - 1) {
            *out = sc_tool_result_fail("path too long", 13);
            return SC_OK;
        }
        memcpy(full_path_buf, c->workspace_dir, n);
        if (n > 0 && full_path_buf[n - 1] != '/' && full_path_buf[n - 1] != '\\')
            full_path_buf[n++] = '/';
        size_t plen = strlen(path);
        if (n + plen >= SC_PATH_BUF) {
            *out = sc_tool_result_fail("path too long", 13);
            return SC_OK;
        }
        memcpy(full_path_buf + n, path, plen + 1);
        full_path = full_path_buf;
    } else {
        if (!c->policy || !c->policy->allowed_paths || c->policy->allowed_paths_count == 0) {
            *out =
                sc_tool_result_fail("Absolute paths not allowed (no allowed_paths configured)", 53);
            return SC_OK;
        }
    }

#ifndef _WIN32
    char *resolved = realpath(full_path, NULL);
    if (!resolved) {
        char *err_msg = sc_sprintf(alloc, "Failed to resolve file path: %s", strerror(errno));
        if (err_msg) {
            *out = sc_tool_result_fail_owned(err_msg, strlen(err_msg));
        } else {
            *out = sc_tool_result_fail("Failed to resolve file path", 27);
        }
        return SC_OK;
    }
#else
    char resolved_buf[SC_PATH_BUF];
    if (_fullpath(resolved_buf, full_path, sizeof(resolved_buf)) == NULL) {
        *out = sc_tool_result_fail("Failed to resolve file path", 27);
        return SC_OK;
    }
    char *resolved = sc_strdup(alloc, resolved_buf);
    if (!resolved) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
#endif

    char *ws_resolved = NULL;
    if (c->workspace_dir && c->workspace_dir_len > 0) {
#ifndef _WIN32
        char ws_buf[SC_PATH_BUF];
        size_t wlen = c->workspace_dir_len;
        if (wlen >= sizeof(ws_buf))
            wlen = sizeof(ws_buf) - 1;
        memcpy(ws_buf, c->workspace_dir, wlen);
        ws_buf[wlen] = '\0';
        char *wr = realpath(ws_buf, NULL);
        if (wr) {
            ws_resolved = sc_strdup(alloc, wr);
            free(wr);
        }
#else
        ws_resolved = sc_strndup(alloc, c->workspace_dir, c->workspace_dir_len);
#endif
    }
    const char *ws = ws_resolved ? ws_resolved : "";
    const char *const *allowed =
        (c->policy && c->policy->allowed_paths) ? c->policy->allowed_paths : NULL;
    size_t allowed_count = c->policy ? c->policy->allowed_paths_count : 0;
    if (!sc_path_resolved_allowed(alloc, resolved, ws, allowed, allowed_count)) {
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        if (ws_resolved)
            alloc->free(alloc->ctx, ws_resolved, strlen(ws_resolved) + 1);
        *out = sc_tool_result_fail("Path is outside allowed areas", 29);
        return SC_OK;
    }
    if (ws_resolved)
        alloc->free(alloc->ctx, ws_resolved, strlen(ws_resolved) + 1);

    /* Read file */
    FILE *f = fopen(resolved, "rb");
    if (!f) {
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        char *err_msg = sc_sprintf(alloc, "Failed to open file: %s", strerror(errno));
        if (err_msg)
            *out = sc_tool_result_fail_owned(err_msg, strlen(err_msg));
        else
            *out = sc_tool_result_fail("Failed to open file", 19);
        return SC_OK;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("Failed to seek file", 18);
        return SC_OK;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > SC_FILE_EDIT_MAX_SIZE) {
        fclose(f);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("File too large or unreadable", 28);
        return SC_OK;
    }
    rewind(f);
    size_t content_len = (size_t)sz;
    char *contents = (char *)alloc->alloc(alloc->ctx, content_len + 1);
    if (!contents) {
        fclose(f);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t read_n = fread(contents, 1, content_len, f);
    fclose(f);
    contents[read_n] = '\0';
    if (read_n != content_len) {
        alloc->free(alloc->ctx, contents, content_len + 1);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("Failed to read file", 18);
        return SC_OK;
    }

    char *pos = strstr(contents, old_text);
    if (!pos) {
        alloc->free(alloc->ctx, contents, content_len + 1);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("old_text not found in file", 25);
        return SC_OK;
    }

    size_t before_len = (size_t)(pos - contents);
    size_t after_len = content_len - before_len - old_len;
    size_t new_len = strlen(new_text);
    size_t total_new = before_len + new_len + after_len;
    char *new_contents = (char *)alloc->alloc(alloc->ctx, total_new + 1);
    if (!new_contents) {
        alloc->free(alloc->ctx, contents, content_len + 1);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(new_contents, contents, before_len);
    memcpy(new_contents + before_len, new_text, new_len + 1);
    memcpy(new_contents + before_len + new_len, pos + old_len, after_len + 1);
    alloc->free(alloc->ctx, contents, content_len + 1);

    /* Atomic write: temp file + rename */
    char tmp_path_buf[SC_PATH_BUF + 16];
#ifndef _WIN32
    int nwr = snprintf(tmp_path_buf, sizeof(tmp_path_buf), "%s.XXXXXX", resolved);
    if (nwr < 0 || (size_t)nwr >= sizeof(tmp_path_buf)) {
        alloc->free(alloc->ctx, new_contents, total_new + 1);
        free(resolved);
        *out = sc_tool_result_fail("path too long", 13);
        return SC_OK;
    }
    int fd = mkstemp(tmp_path_buf);
    if (fd < 0) {
        alloc->free(alloc->ctx, new_contents, total_new + 1);
        free(resolved);
        char *err_msg = sc_sprintf(alloc, "Failed to create temp file: %s", strerror(errno));
        if (err_msg)
            *out = sc_tool_result_fail_owned(err_msg, strlen(err_msg));
        else
            *out = sc_tool_result_fail("Failed to create temp file", 24);
        return SC_OK;
    }
    FILE *tf = fdopen(fd, "wb");
#else
    int nwr = snprintf(tmp_path_buf, sizeof(tmp_path_buf), "%s.tmp", resolved);
    if (nwr < 0 || (size_t)nwr >= sizeof(tmp_path_buf)) {
        alloc->free(alloc->ctx, new_contents, total_new + 1);
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
        *out = sc_tool_result_fail("path too long", 13);
        return SC_OK;
    }
    FILE *tf = fopen(tmp_path_buf, "wb");
#endif
    if (!tf) {
        alloc->free(alloc->ctx, new_contents, total_new + 1);
#ifndef _WIN32
        free(resolved);
#else
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("Failed to open temp file", 24);
        return SC_OK;
    }
    size_t written = fwrite(new_contents, 1, total_new, tf);
    fflush(tf);
    fclose(tf);
    alloc->free(alloc->ctx, new_contents, total_new + 1);
    if (written != total_new) {
#ifndef _WIN32
        unlink(tmp_path_buf);
        free(resolved);
#else
        remove(tmp_path_buf);
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif
        *out = sc_tool_result_fail("Failed to write file", 19);
        return SC_OK;
    }
#ifndef _WIN32
    if (rename(tmp_path_buf, resolved) != 0) {
        unlink(tmp_path_buf);
        free(resolved);
        char *err_msg = sc_sprintf(alloc, "Failed to rename temp file: %s", strerror(errno));
        if (err_msg)
            *out = sc_tool_result_fail_owned(err_msg, strlen(err_msg));
        else
            *out = sc_tool_result_fail("Failed to rename temp file", 27);
        return SC_OK;
    }
    free(resolved);
#else
    if (remove(resolved) != 0) { /* Delete original first on Windows */
        remove(tmp_path_buf);
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
        *out = sc_tool_result_fail("Failed to replace file", 21);
        return SC_OK;
    }
    if (rename(tmp_path_buf, resolved) != 0) {
        alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
        *out = sc_tool_result_fail("Failed to rename temp file", 27);
        return SC_OK;
    }
    alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
#endif

    char *msg = sc_sprintf(alloc, "Replaced %zu bytes with %zu bytes in %s", (size_t)old_len,
                           new_len, path);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, strlen(msg));
    return SC_OK;
#endif
}

static const char *file_edit_name(void *ctx) {
    (void)ctx;
    return SC_FILE_EDIT_NAME;
}
static const char *file_edit_desc(void *ctx) {
    (void)ctx;
    return SC_FILE_EDIT_DESC;
}
static const char *file_edit_params(void *ctx) {
    (void)ctx;
    return SC_FILE_EDIT_PARAMS;
}

static void file_edit_deinit(void *ctx, sc_allocator_t *alloc) {
    if (!ctx)
        return;
    sc_file_edit_ctx_t *c = (sc_file_edit_ctx_t *)ctx;
    if (c->workspace_dir && alloc)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    free(c);
}

static const sc_tool_vtable_t file_edit_vtable = {
    .execute = file_edit_execute,
    .name = file_edit_name,
    .description = file_edit_desc,
    .parameters_json = file_edit_params,
    .deinit = file_edit_deinit,
};

sc_error_t sc_file_edit_create(sc_allocator_t *alloc, const char *workspace_dir,
                               size_t workspace_dir_len, sc_security_policy_t *policy,
                               sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_file_edit_ctx_t *c = (sc_file_edit_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = sc_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &file_edit_vtable;
    return SC_OK;
}
