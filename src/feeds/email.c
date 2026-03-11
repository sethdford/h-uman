/* Phase 7 email feed. F92. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/email.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

#define HU_EMAIL_SCRIPT_NAME "email_query.applescript"

#if HU_IS_TEST

hu_error_t hu_email_fetch_unread(hu_allocator_t *alloc,
    hu_email_header_t *headers, size_t cap, size_t *out_count) {
    (void)alloc;
    if (!headers || !out_count || cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(headers, 0, sizeof(hu_email_header_t) * 2);
    (void)strncpy(headers[0].subject, "Meeting Tomorrow",
        sizeof(headers[0].subject) - 1);
    (void)strncpy(headers[0].from, "alice@example.com",
        sizeof(headers[0].from) - 1);
    headers[0].date = (int64_t)time(NULL);
    (void)strncpy(headers[1].subject, "Project Update",
        sizeof(headers[1].subject) - 1);
    (void)strncpy(headers[1].from, "bob@example.com",
        sizeof(headers[1].from) - 1);
    headers[1].date = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

#if defined(__APPLE__) && defined(HU_GATEWAY_POSIX)
static int resolve_script_path(const char *script_name, char *buf, size_t cap) {
    buf[0] = '\0';
    const char *root = getenv("HU_PROJECT_ROOT");
    if (root && root[0]) {
        int n = snprintf(buf, cap, "%s/scripts/%s", root, script_name);
        if (n > 0 && (size_t)n < cap)
            return 0;
    }
    char exe_buf[4096];
    uint32_t size = (uint32_t)sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        char *resolved = realpath(exe_buf, NULL);
        const char *exe = resolved ? resolved : exe_buf;
        const char *build = strstr(exe, "/build");
        if (build && build > exe) {
            size_t prefix = (size_t)(build - exe);
            if (prefix < cap - 64) {
                memcpy(buf, exe, prefix);
                buf[prefix] = '\0';
                snprintf(buf + prefix, cap - prefix, "/scripts/%s", script_name);
                if (resolved)
                    free(resolved);
                return 0;
            }
        }
        if (resolved)
            free(resolved);
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(buf, cap, "%s/scripts/%s", cwd, script_name);
        return 0;
    }
    return -1;
}

static void parse_pipe_line(const char *line, char *out_subject, size_t subj_cap,
    char *out_from, size_t from_cap, int64_t *out_date) {
    out_subject[0] = '\0';
    out_from[0] = '\0';
    *out_date = 0;
    const char *p = line;
    const char *start = p;
    int field = 0;
    while (*p) {
        if (*p == '|') {
            size_t len = (size_t)(p - start);
            if (field == 0) {
                if (len >= subj_cap)
                    len = subj_cap - 1;
                memcpy(out_subject, start, len);
                out_subject[len] = '\0';
            } else if (field == 1) {
                if (len >= from_cap)
                    len = from_cap - 1;
                memcpy(out_from, start, len);
                out_from[len] = '\0';
            }
            field++;
            start = p + 1;
        }
        p++;
    }
    if (field >= 2) {
        size_t len = (size_t)(p - start);
        char buf[32];
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, start, len);
        buf[len] = '\0';
        *out_date = (int64_t)atoll(buf);
    }
}
#endif

hu_error_t hu_email_fetch_unread(hu_allocator_t *alloc,
    hu_email_header_t *headers, size_t cap, size_t *out_count) {
    if (!alloc || !headers || !out_count || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if !defined(__APPLE__) || !defined(HU_GATEWAY_POSIX)
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    char script_path[4096];
    if (resolve_script_path(HU_EMAIL_SCRIPT_NAME, script_path, sizeof(script_path)) != 0)
        return HU_ERR_NOT_FOUND;
    char *script_dup = hu_strdup(alloc, script_path);
    if (!script_dup)
        return HU_ERR_OUT_OF_MEMORY;
    const char *argv[] = {"osascript", script_dup, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &result);
    alloc->free(alloc->ctx, script_dup, strlen(script_path) + 1);
    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        return HU_OK;
    }
    const char *p = result.stdout_buf;
    size_t n = 0;
    while (n < cap && *p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        if (eol > p) {
            char line[1024];
            size_t line_len = (size_t)(eol - p);
            if (line_len >= sizeof(line))
                line_len = sizeof(line) - 1;
            memcpy(line, p, line_len);
            line[line_len] = '\0';
            memset(&headers[n], 0, sizeof(headers[n]));
            parse_pipe_line(line,
                headers[n].subject, sizeof(headers[n].subject),
                headers[n].from, sizeof(headers[n].from),
                &headers[n].date);
            n++;
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    hu_run_result_free(alloc, &result);
    *out_count = n;
    return HU_OK;
#endif
}

#endif /* HU_IS_TEST */

#else
typedef int hu_email_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
