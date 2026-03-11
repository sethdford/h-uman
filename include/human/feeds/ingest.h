#ifndef HU_FEEDS_INGEST_H
#define HU_FEEDS_INGEST_H

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Feed ingestion item (fixed buffers for external feed APIs).
 * Used by social (Facebook/Instagram) and Apple (Photos, Reminders) feeds.
 */
typedef struct hu_feed_ingest_item {
    char source[32];
    char contact_id[128];
    char content_type[32];
    char content[2048];
    size_t content_len;
    char url[512];
    int64_t ingested_at;
} hu_feed_ingest_item_t;

#endif
