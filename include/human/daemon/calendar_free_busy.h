#ifndef HU_DAEMON_CALENDAR_FREE_BUSY_H
#define HU_DAEMON_CALENDAR_FREE_BUSY_H

/*
 * Owner free/busy from macOS Calendar, read through the tiny EventKit helper in
 * tools/calendar-free-busy/ (built at install to ~/.local/bin/hu-calendar-free-busy,
 * or HU_CALENDAR_HELPER). The helper prints ONE JSON object and nothing else:
 *
 *   {"access":"granted","busy":[{"start":1759514400,"end":1759518000}]}
 *   {"access":"denied"}  /  {"access":"undetermined"}
 *
 * Busy intervals only: no titles, notes, locations or attendees ever leave
 * EventKit. Events marked "free" and events the owner declined are not busy.
 *
 * No access, no helper, a timeout or malformed output all read as UNKNOWN; the
 * commitment guard treats an unknown calendar as consequential for plans.
 */

#include "human/core/allocator.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_calendar_state {
    HU_CAL_NOT_CHECKED = 0, /* the commitment needed no calendar */
    HU_CAL_FREE,
    HU_CAL_BUSY,
    HU_CAL_UNKNOWN,
} hu_calendar_state_t;

/* Free/busy for [start, end) unix seconds. */
typedef hu_calendar_state_t (*hu_calendar_query_fn)(void *ctx, int64_t start, int64_t end);

#define HU_CALENDAR_HELPER_TIMEOUT_S 3

/* Pure: the helper's JSON -> state for [start, end). Any busy interval that
 * overlaps the window is BUSY; access other than "granted" or bad JSON is
 * UNKNOWN. */
hu_calendar_state_t hu_calendar_free_busy_parse(hu_allocator_t *alloc, const char *json,
                                                size_t json_len, int64_t start, int64_t end);

/* Production hu_calendar_query_fn: runs the helper (timeout
 * HU_CALENDAR_HELPER_TIMEOUT_S). ctx unused. Test builds never spawn: UNKNOWN. */
hu_calendar_state_t hu_calendar_free_busy_query(void *ctx, int64_t start, int64_t end);

const char *hu_calendar_state_name(hu_calendar_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_CALENDAR_FREE_BUSY_H */
