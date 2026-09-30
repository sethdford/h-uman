#ifndef HU_TOOLS_CALENDAR_H
#define HU_TOOLS_CALENDAR_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
hu_error_t hu_calendar_create(hu_allocator_t *alloc, hu_tool_t *out);

#include <stdbool.h>
#include <stddef.h>

/* The decisions inside the Google Calendar tool, extracted so tests can pin
 * them without a network (the request path is compiled out under
 * HU_IS_TEST). */

/* A calendar or event id is pasted into the request URL, so it must not be
 * able to change the path or query: non-empty, at most `max_len` bytes, and
 * only [A-Za-z0-9._@-]. */
bool hu_calendar_id_is_safe(const char *id, size_t max_len);

/* The JSON body for an event insert, built with the JSON writer so a quote
 * or backslash in the title cannot break or extend the document. `desc` may
 * be NULL. Caller frees *out with alloc->free(ctx, *out, *out_len + 1). */
hu_error_t hu_calendar_event_body(hu_allocator_t *alloc, const char *title, const char *start,
                                  const char *end, const char *desc, char **out, size_t *out_len);

/* Google answers a successful insert with 200 and a delete with 204. Any
 * other status is a failure, whatever the transport said. */
bool hu_calendar_status_ok(long status_code);
#endif
