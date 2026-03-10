#ifndef HU_EVENT_EXTRACT_H
#define HU_EVENT_EXTRACT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#define HU_EVENT_MAX_EVENTS 10

typedef struct hu_extracted_event {
    char *description;     /* "interview", "birthday", "doctor's appointment" */
    size_t description_len;
    char *temporal_ref;    /* "Tuesday", "next week", "March 15th" */
    size_t temporal_ref_len;
    double confidence;     /* 0.0-1.0 */
} hu_extracted_event_t;

typedef struct hu_event_extract_result {
    hu_extracted_event_t events[HU_EVENT_MAX_EVENTS];
    size_t event_count;
} hu_event_extract_result_t;

hu_error_t hu_event_extract(hu_allocator_t *alloc, const char *text, size_t text_len,
                            hu_event_extract_result_t *out);
void hu_event_extract_result_deinit(hu_event_extract_result_t *result, hu_allocator_t *alloc);

#endif
