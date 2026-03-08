#ifndef SC_EVENT_EXTRACT_H
#define SC_EVENT_EXTRACT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

#define SC_EVENT_MAX_EVENTS 10

typedef struct sc_extracted_event {
    char *description;     /* "interview", "birthday", "doctor's appointment" */
    size_t description_len;
    char *temporal_ref;    /* "Tuesday", "next week", "March 15th" */
    size_t temporal_ref_len;
    double confidence;     /* 0.0-1.0 */
} sc_extracted_event_t;

typedef struct sc_event_extract_result {
    sc_extracted_event_t events[SC_EVENT_MAX_EVENTS];
    size_t event_count;
} sc_event_extract_result_t;

sc_error_t sc_event_extract(sc_allocator_t *alloc, const char *text, size_t text_len,
                            sc_event_extract_result_t *out);
void sc_event_extract_result_deinit(sc_event_extract_result_t *result, sc_allocator_t *alloc);

#endif
