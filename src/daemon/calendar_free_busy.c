/* Owner free/busy via the EventKit helper. Contract:
 * include/human/daemon/calendar_free_busy.h; helper: tools/calendar-free-busy/. */
#include "human/daemon/calendar_free_busy.h"

#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/paths.h"
#include "human/core/process_util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HU_CALENDAR_HELPER_NAME "hu-calendar-free-busy"
#define HU_CALENDAR_OUTPUT_MAX  65536

const char *hu_calendar_state_name(hu_calendar_state_t s) {
    switch (s) {
    case HU_CAL_FREE:
        return "free";
    case HU_CAL_BUSY:
        return "busy";
    case HU_CAL_UNKNOWN:
        return "unknown";
    default:
        return "n/a";
    }
}

hu_calendar_state_t hu_calendar_free_busy_parse(hu_allocator_t *alloc, const char *json,
                                                size_t json_len, int64_t start, int64_t end) {
    if (!alloc || !json || json_len == 0 || end <= start)
        return HU_CAL_UNKNOWN;
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, json, json_len, &root) != HU_OK || !root)
        return HU_CAL_UNKNOWN;
    hu_calendar_state_t st = HU_CAL_UNKNOWN;
    const char *access = root->type == HU_JSON_OBJECT ? hu_json_get_string(root, "access") : NULL;
    hu_json_value_t *busy = access ? hu_json_object_get(root, "busy") : NULL;
    if (access && strcmp(access, "granted") == 0 && busy && busy->type == HU_JSON_ARRAY) {
        st = HU_CAL_FREE;
        for (size_t i = 0; i < busy->data.array.len; i++) {
            const hu_json_value_t *b = busy->data.array.items[i];
            if (!b || b->type != HU_JSON_OBJECT) {
                st = HU_CAL_UNKNOWN; /* a malformed row means we can't vouch for "free" */
                break;
            }
            int64_t bs = (int64_t)hu_json_get_number(b, "start", 0);
            int64_t be = (int64_t)hu_json_get_number(b, "end", 0);
            if (be > bs && bs < end && be > start) {
                st = HU_CAL_BUSY;
                break;
            }
        }
    }
    hu_json_free(alloc, root);
    return st;
}

#if !defined(HU_IS_TEST) || !HU_IS_TEST
/* $HU_CALENDAR_HELPER, else ~/.local/bin/hu-calendar-free-busy (where
 * scripts/install-human-daemon.sh puts it). */
static bool calendar_helper_path(char *buf, size_t cap) {
    const char *env = getenv("HU_CALENDAR_HELPER");
    if (env && env[0] == '/') {
        int n = snprintf(buf, cap, "%s", env);
        return n > 0 && (size_t)n < cap;
    }
    char home[512];
    int h = hu_paths_home(home, sizeof(home));
    if (h < 0 || (size_t)h >= sizeof(home))
        return false;
    int n = snprintf(buf, cap, "%s/.local/bin/" HU_CALENDAR_HELPER_NAME, home);
    return n > 0 && (size_t)n < cap;
}
#endif

hu_calendar_state_t hu_calendar_free_busy_query(void *ctx, int64_t start, int64_t end) {
    (void)ctx;
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)start;
    (void)end;
    return HU_CAL_UNKNOWN; /* tests inject a hu_calendar_query_fn; never spawn */
#else
    char path[1024];
    if (!calendar_helper_path(path, sizeof(path)) || access(path, X_OK) != 0)
        return HU_CAL_UNKNOWN;
    char s_arg[32], e_arg[32];
    snprintf(s_arg, sizeof(s_arg), "%" PRId64, start);
    snprintf(e_arg, sizeof(e_arg), "%" PRId64, end);
    /* --prompt: when access is undetermined the helper asks macOS once (the
     * owner's one-time permission prompt) and still answers "undetermined". */
    const char *argv[] = {path, "--start", s_arg, "--end", e_arg, "--prompt", NULL};
    hu_allocator_t alloc = hu_system_allocator();
    hu_run_result_t r;
    memset(&r, 0, sizeof(r));
    hu_error_t err = hu_process_run_with_timeout(&alloc, argv, NULL, HU_CALENDAR_OUTPUT_MAX,
                                                 HU_CALENDAR_HELPER_TIMEOUT_S, &r);
    hu_calendar_state_t st = HU_CAL_UNKNOWN;
    if (err == HU_OK && r.success && r.stdout_buf)
        st = hu_calendar_free_busy_parse(&alloc, r.stdout_buf, r.stdout_len, start, end);
    else
        hu_log_warn("calendar", NULL, "free/busy helper failed (err=%d exit=%d)", (int)err,
                    r.exit_code);
    hu_run_result_free(&alloc, &r);
    return st;
#endif
}
