#ifndef SC_MULTIMODAL_CALENDAR_H
#define SC_MULTIMODAL_CALENDAR_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define SC_CALENDAR_MAX_EVENTS 32

typedef enum sc_event_type {
    SC_EVENT_MEETING,
    SC_EVENT_BIRTHDAY,
    SC_EVENT_REMINDER,
    SC_EVENT_DEADLINE,
    SC_EVENT_SOCIAL,
    SC_EVENT_OTHER,
} sc_event_type_t;

typedef struct sc_calendar_event {
    char *title;
    size_t title_len;
    sc_event_type_t type;
    int64_t start_ts;
    int64_t end_ts;
    char *location;
    size_t location_len;
    char *contact; /* associated contact, if any */
    size_t contact_len;
} sc_calendar_event_t;

typedef struct sc_calendar_context {
    sc_calendar_event_t events[SC_CALENDAR_MAX_EVENTS];
    size_t event_count;
} sc_calendar_context_t;

/* Detect event type from title keywords */
sc_event_type_t sc_calendar_detect_type(const char *title, size_t title_len);

/* Add an event to context */
sc_error_t sc_calendar_add_event(sc_calendar_context_t *ctx, sc_allocator_t *alloc,
                                 const char *title, size_t title_len, sc_event_type_t type,
                                 int64_t start_ts, int64_t end_ts);

/* Build prompt context for upcoming events */
sc_error_t sc_calendar_build_context(const sc_calendar_context_t *ctx, sc_allocator_t *alloc,
                                     int64_t now_ts, char **out, size_t *out_len);

/* Free all events in context */
void sc_calendar_deinit(sc_calendar_context_t *ctx, sc_allocator_t *alloc);

#endif
