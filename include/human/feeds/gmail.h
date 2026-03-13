#ifndef HU_FEEDS_GMAIL_H
#define HU_FEEDS_GMAIL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/feeds/ingest.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

/**
 * Fetch recent Gmail messages via the Gmail REST API (OAuth2).
 * Reuses the same credential scheme as the Gmail channel.
 *
 * client_id, client_secret, refresh_token: Google OAuth2 credentials.
 * items/items_cap: output buffer for ingested feed items.
 * out_count: number of items written.
 */
hu_error_t hu_gmail_feed_fetch(hu_allocator_t *alloc,
    const char *client_id, size_t client_id_len,
    const char *client_secret, size_t client_secret_len,
    const char *refresh_token, size_t refresh_token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
