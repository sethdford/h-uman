#ifndef HU_FEEDS_MUSIC_H
#define HU_FEEDS_MUSIC_H

#include "human/core/allocator.h"
#include "human/feeds/ingest.h"
#include <sqlite3.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

hu_error_t hu_spotify_fetch_recent(hu_allocator_t *alloc, sqlite3 *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
