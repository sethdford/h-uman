#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/platform.h"
#include "human/platform/calendar.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

hu_error_t hu_calendar_macos_get_events(hu_allocator_t *alloc, int hours_ahead, char **events_json,
                                        size_t *events_len) {
    if (!alloc || !events_json || !events_len)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    (void)hours_ahead;
    *events_json = (char *)alloc->alloc(alloc->ctx, 3);
    if (!*events_json)
        return HU_ERR_OUT_OF_MEMORY;
    (*events_json)[0] = '[';
    (*events_json)[1] = ']';
    (*events_json)[2] = '\0';
    *events_len = 2;
    return HU_OK;
#else

#if !defined(__APPLE__)
    (void)hours_ahead;
    return HU_ERR_NOT_SUPPORTED;
#else

    /* Resolve script path: try HU_PROJECT_ROOT, then derive from exe, else cwd */
    char script_path[4096];
    script_path[0] = '\0';
    const char *root = getenv("HU_PROJECT_ROOT");
    if (root && root[0]) {
        int n = snprintf(script_path, sizeof(script_path), "%s/scripts/calendar_query.applescript",
                         root);
        if (n <= 0 || (size_t)n >= sizeof(script_path))
            root = NULL;
    }
    if (!root || !root[0]) {
        char exe_buf[4096];
        uint32_t size = (uint32_t)sizeof(exe_buf);
        if (_NSGetExecutablePath(exe_buf, &size) == 0) {
            char *resolved = hu_platform_realpath(alloc, exe_buf);
            const char *exe = resolved ? resolved : exe_buf;
            const char *build = strstr(exe, "/build");
            if (build && build > exe) {
                size_t prefix = (size_t)(build - exe);
                if (prefix < sizeof(script_path) - 50) {
                    memcpy(script_path, exe, prefix);
                    script_path[prefix] = '\0';
                    snprintf(script_path + prefix, sizeof(script_path) - prefix,
                             "/scripts/calendar_query.applescript");
                }
            }
            if (resolved)
                alloc->free(alloc->ctx, resolved, strlen(resolved) + 1);
        }
    }
    if (script_path[0] == '\0') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(script_path, sizeof(script_path), "%s/scripts/calendar_query.applescript",
                     cwd);
        }
    }
    if (script_path[0] == '\0')
        return HU_ERR_NOT_FOUND;

    char hours_buf[16];
    int hw = snprintf(hours_buf, sizeof(hours_buf), "%d", hours_ahead > 0 ? hours_ahead : 24);
    if (hw <= 0 || (size_t)hw >= sizeof(hours_buf))
        return HU_ERR_INVALID_ARGUMENT;

    const char *script_dup = hu_strdup(alloc, script_path);
    if (!script_dup)
        return HU_ERR_OUT_OF_MEMORY;

    const char *argv[] = {"osascript", script_dup, hours_buf, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &result);
    alloc->free(alloc->ctx, (void *)script_dup, strlen(script_path) + 1);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        *events_json = (char *)alloc->alloc(alloc->ctx, 3);
        if (*events_json) {
            (*events_json)[0] = '[';
            (*events_json)[1] = ']';
            (*events_json)[2] = '\0';
            *events_len = 2;
            return HU_OK; /* graceful degradation */
        }
        return err;
    }

    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        *events_json = (char *)alloc->alloc(alloc->ctx, 3);
        if (*events_json) {
            (*events_json)[0] = '[';
            (*events_json)[1] = ']';
            (*events_json)[2] = '\0';
            *events_len = 2;
            return HU_OK;
        }
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t len = result.stdout_len;
    while (len > 0 && (result.stdout_buf[len - 1] == '\n' || result.stdout_buf[len - 1] == '\r'))
        len--;
    *events_json = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!*events_json) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(*events_json, result.stdout_buf, len);
    (*events_json)[len] = '\0';
    *events_len = len;
    hu_run_result_free(alloc, &result);
    return HU_OK;

#endif /* __APPLE__ */
#endif /* HU_IS_TEST */
}
