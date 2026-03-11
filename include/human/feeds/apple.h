#ifndef HU_FEEDS_APPLE_H
#define HU_FEEDS_APPLE_H

#include "human/core/allocator.h"
#include "human/feeds/ingest.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

hu_error_t hu_apple_photos_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

hu_error_t hu_apple_contacts_fetch(hu_allocator_t *alloc,
    char *out_json, size_t out_cap, size_t *out_len);

hu_error_t hu_apple_reminders_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

typedef struct hu_health_summary {
    int32_t steps_today;
    double avg_heart_rate;
    double sleep_hours;
    int64_t export_date;
} hu_health_summary_t;

hu_error_t hu_apple_health_parse_export(hu_allocator_t *alloc,
    const char *xml_data, size_t xml_len,
    hu_health_summary_t *out);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
