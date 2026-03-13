#ifndef HU_FEEDS_FILE_INGEST_H
#define HU_FEEDS_FILE_INGEST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/feeds/ingest.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

/**
 * Read JSONL files from the ingest directory (~/.human/feeds/ingest/).
 * Each line is a JSON object with: source, content_type, content, url (optional).
 * Files are renamed to .done after processing to avoid re-ingestion.
 *
 * items/items_cap: output buffer for ingested feed items.
 * out_count: number of items written.
 */
hu_error_t hu_file_ingest_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
