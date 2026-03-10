#ifndef HU_MULTIMODAL_CALENDAR_H
#define HU_MULTIMODAL_CALENDAR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_CALENDAR_MAX_EVENTS 32

typedef enum hu_event_type {
    HU_EVENT_MEETING,
    HU_EVENT_BIRTHDAY,
    HU_EVENT_REMINDER,
    HU_EVENT_DEADLINE,
    HU_EVENT_SOCIAL,
    HU_EVENT_OTHER,
} hu_event_type_t;

typedef struct hu_calendar_event {
    char *title;
    size_t title_len;
    hu_event_type_t type;
    int64_t start_ts;
    int64_t end_ts;
    char *location;
    size_t location_len;
    char *contact; /* associated contact, if any */
    size_t contact_len;
} hu_calendar_event_t;

typedef struct hu_calendar_context {
    hu_calendar_event_t events[HU_CALENDAR_MAX_EVENTS];
    size_t event_count;
} hu_calendar_context_t;

/* Detect event type from title keywords */
hu_event_type_t hu_calendar_detect_type(const char *title, size_t title_len);

/* Add an event to context */
hu_error_t hu_calendar_add_event(hu_calendar_context_t *ctx, hu_allocator_t *alloc,
                                 const char *title, size_t title_len, hu_event_type_t type,
                                 int64_t start_ts, int64_t end_ts);

/* Build prompt context for upcoming events */
hu_error_t hu_calendar_build_context(const hu_calendar_context_t *ctx, hu_allocator_t *alloc,
                                     int64_t now_ts, char **out, size_t *out_len);

/* Free all events in context */
void hu_calendar_deinit(hu_calendar_context_t *ctx, hu_allocator_t *alloc);

#endif
