#ifndef HU_FEEDS_SOCIAL_H
#define HU_FEEDS_SOCIAL_H

#include "human/core/allocator.h"
#include "human/feeds/ingest.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_SOCIAL

hu_error_t hu_social_fetch_facebook(hu_allocator_t *alloc, void *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

hu_error_t hu_social_fetch_instagram(hu_allocator_t *alloc, void *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

#endif /* HU_ENABLE_SOCIAL */

#ifdef __cplusplus
}
#endif

#endif
