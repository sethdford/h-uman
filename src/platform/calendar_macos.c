#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/platform.h"
#include "human/platform/calendar.h"
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Events starting within the window, plus all-day events in progress, as
 * [{"name","start","h","m","allday"}]. Titles are JSON-escaped in the
 * script; h/m are numbers so callers need not parse a locale's date string. */
#if defined(__APPLE__) && !defined(HU_IS_TEST)
static const char *const k_calendar_script[] = {
    "on replaceText(t, a, b)",
    "set AppleScript's text item delimiters to a",
    "set parts to text items of t",
    "set AppleScript's text item delimiters to b",
    "set t to parts as text",
    "set AppleScript's text item delimiters to \"\"",
    "return t",
    "end replaceText",
    "on esc(t)",
    "set t to t as text",
    "set t to my replaceText(t, \"\\\\\", \"\\\\\\\\\")",
    "set t to my replaceText(t, \"\\\"\", \"\\\\\\\"\")",
    "set t to my replaceText(t, return, \"\\\\n\")",
    "set t to my replaceText(t, linefeed, \"\\\\n\")",
    "set t to my replaceText(t, tab, \" \")",
    "return t",
    "end esc",
    "on run argv",
    "set hoursAhead to 24",
    "if (count of argv) > 0 then set hoursAhead to item 1 of argv as integer",
    "set output to \"[\"",
    "set isFirst to true",
    "tell application \"Calendar\"",
    "set nowDate to current date",
    "set endDate to nowDate + (hoursAhead * hours)",
    "repeat with cal in calendars",
    "set calEvents to (every event of cal whose (start date >= nowDate and start date <= endDate) "
    "or (allday event is true and start date <= nowDate and end date > nowDate))",
    "repeat with ev in calEvents",
    "set evStart to start date of ev",
    "set evLine to \"{\\\"name\\\":\\\"\" & my esc(summary of ev) & \"\\\",\\\"start\\\":\\\"\" & "
    "my esc(evStart as string) & \"\\\",\\\"h\\\":\" & (hours of evStart) & \",\\\"m\\\":\" & "
    "(minutes of evStart) & \",\\\"allday\\\":\" & (allday event of ev) & \"}\"",
    "if not isFirst then set output to output & \",\"",
    "set output to output & evLine",
    "set isFirst to false",
    "end repeat",
    "end repeat",
    "end tell",
    "return output & \"]\"",
    "end run",
};
#endif

#if defined(__APPLE__) || defined(HU_IS_TEST)
static hu_error_t empty_array(hu_allocator_t *alloc, char **events_json, size_t *events_len) {
    *events_json = (char *)alloc->alloc(alloc->ctx, 3);
    if (!*events_json)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*events_json, "[]", 3);
    *events_len = 2;
    return HU_OK;
}
#endif

hu_error_t hu_calendar_macos_get_events(hu_allocator_t *alloc, int hours_ahead, char **events_json,
                                        size_t *events_len) {
    if (!alloc || !events_json || !events_len)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    (void)hours_ahead;
    return empty_array(alloc, events_json, events_len);
#else

#if !defined(__APPLE__)
    (void)hours_ahead;
    return HU_ERR_NOT_SUPPORTED;
#else
    /* The script travels inside the binary as `osascript -e` lines, so an
     * installed daemon needs no scripts/ directory beside it. */
    enum { N_LINES = sizeof(k_calendar_script) / sizeof(k_calendar_script[0]) };
    const char *argv[1 + 2 * N_LINES + 2];
    size_t ai = 0;
    argv[ai++] = "osascript";
    for (size_t i = 0; i < N_LINES; i++) {
        argv[ai++] = "-e";
        argv[ai++] = k_calendar_script[i];
    }
    char hours_buf[16];
    snprintf(hours_buf, sizeof(hours_buf), "%d", hours_ahead > 0 ? hours_ahead : 24);
    argv[ai++] = hours_buf;
    argv[ai] = NULL;

    hu_run_result_t result = {0};
    /* Bounded: a Calendar permission prompt must not stall the service loop. */
    hu_error_t err = hu_process_run_with_timeout(alloc, argv, NULL, 262144, 20, &result);
    if (err != HU_OK || !result.success || !result.stdout_buf) {
        static atomic_bool warned = false;
        hu_log_warn_once(&warned, "calendar", NULL,
                         "Calendar query failed (%s, exit %d); treating the calendar as empty. "
                         "Check that the daemon has Calendar access in System Settings > Privacy.",
                         err != HU_OK ? hu_error_string(err) : "osascript", result.exit_code);
        hu_run_result_free(alloc, &result);
        return empty_array(alloc, events_json, events_len);
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
