#ifndef HU_FEEDS_TWITTER_H
#define HU_FEEDS_TWITTER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/feeds/ingest.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

/**
 * Fetch recent tweets from the user's home timeline via Twitter API v2.
 * Requires Bearer token with at least Basic API access ($100/month).
 *
 * bearer_token: Twitter API v2 bearer token.
 * items/items_cap: output buffer for ingested feed items.
 * out_count: number of items written.
 */
hu_error_t hu_twitter_feed_fetch(hu_allocator_t *alloc,
    const char *bearer_token, size_t bearer_token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
