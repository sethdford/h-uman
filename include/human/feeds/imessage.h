#ifndef HU_FEEDS_IMESSAGE_H
#define HU_FEEDS_IMESSAGE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/feeds/ingest.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

/**
 * Fetch recent iMessage conversations from ~/Library/Messages/chat.db.
 * macOS only — returns HU_ERR_NOT_SUPPORTED on other platforms.
 *
 * since_epoch: only fetch messages newer than this Unix timestamp.
 * items/items_cap: output buffer for ingested feed items.
 * out_count: number of items written.
 */
hu_error_t hu_imessage_feed_fetch(hu_allocator_t *alloc, int64_t since_epoch,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
