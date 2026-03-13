#ifndef HU_FEEDS_PROCESSOR_H
#define HU_FEEDS_PROCESSOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_feed_type {
    HU_FEED_SOCIAL_FACEBOOK = 0,   /* F83 */
    HU_FEED_SOCIAL_INSTAGRAM,      /* F84 */
    HU_FEED_APPLE_PHOTOS,          /* F85 */
    HU_FEED_GOOGLE_PHOTOS,         /* F86 */
    HU_FEED_APPLE_CONTACTS,        /* F87 */
    HU_FEED_APPLE_REMINDERS,       /* F88 */
    HU_FEED_MUSIC,                 /* F89 */
    HU_FEED_NEWS_RSS,              /* F90 */
    HU_FEED_APPLE_HEALTH,          /* F91 */
    HU_FEED_EMAIL,                 /* F92 */
    HU_FEED_GMAIL,                 /* F94 — Gmail API via OAuth */
    HU_FEED_IMESSAGE,              /* F95 — iMessage via chat.db */
    HU_FEED_TWITTER,               /* F96 — Twitter/X timeline API */
    HU_FEED_TIKTOK,                /* F97 — TikTok via file ingest */
    HU_FEED_FILE_INGEST,           /* F98 — generic JSONL file ingest */
    HU_FEED_COUNT
} hu_feed_type_t;

typedef struct hu_feed_item {
    int64_t id;
    hu_feed_type_t type;
    char *source;
    size_t source_len; /* "instagram:@friend", "rss:techcrunch" */
    char *content;
    size_t content_len;
    char *topic;
    size_t topic_len;
    double relevance;
    uint64_t fetched_at;
    bool processed;
} hu_feed_item_t;

typedef struct hu_feed_config {
    bool enabled[HU_FEED_COUNT];
    uint32_t poll_interval_minutes[HU_FEED_COUNT];
    uint32_t max_items_per_poll;
} hu_feed_config_t;

hu_error_t hu_feeds_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_feeds_insert_sql(const hu_feed_item_t *item, char *buf, size_t cap,
                               size_t *out_len);
hu_error_t hu_feeds_query_unprocessed_sql(hu_feed_type_t type, uint32_t limit,
                                          char *buf, size_t cap, size_t *out_len);
hu_error_t hu_feeds_mark_processed_sql(int64_t id, char *buf, size_t cap,
                                       size_t *out_len);
hu_error_t hu_feeds_query_by_topic_sql(const char *topic, size_t topic_len,
                                      uint32_t limit, char *buf, size_t cap,
                                      size_t *out_len);

/* F93 — Feed processing daemon: determine which feeds need polling */
bool hu_feeds_should_poll(hu_feed_type_t type, const hu_feed_config_t *config,
                          uint64_t last_poll_ms, uint64_t now_ms);
const char *hu_feed_type_str(hu_feed_type_t type);
double hu_feeds_score_relevance(const char *content, size_t content_len,
                                const char *interest, size_t interest_len);
hu_error_t hu_feeds_build_prompt(hu_allocator_t *alloc,
                                 const hu_feed_item_t *items, size_t count,
                                 char **out, size_t *out_len);
void hu_feed_item_deinit(hu_allocator_t *alloc, hu_feed_item_t *item);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

typedef struct hu_feed_processor {
    hu_allocator_t *alloc;
    sqlite3 *db;
} hu_feed_processor_t;

typedef struct hu_feed_item_stored {
    char source[32];
    char contact_id[128];
    char content_type[32];
    char content[2048];
    size_t content_len;
    char url[512];
    int64_t ingested_at;
} hu_feed_item_stored_t;

hu_error_t hu_feed_processor_store_item(hu_feed_processor_t *proc,
                                        const hu_feed_item_stored_t *item);
hu_error_t hu_feed_processor_get_recent(hu_allocator_t *alloc, sqlite3 *db,
                                        const char *source, size_t src_len,
                                        size_t limit,
                                        hu_feed_item_stored_t **out,
                                        size_t *out_count);
hu_error_t hu_feed_processor_get_for_contact(hu_allocator_t *alloc, sqlite3 *db,
                                             const char *contact_id,
                                             size_t cid_len, size_t limit,
                                             hu_feed_item_stored_t **out,
                                             size_t *out_count);
void hu_feed_items_free(hu_allocator_t *alloc, hu_feed_item_stored_t *items,
                        size_t count);

/* Poll all enabled feeds that are due. Stores new items via hu_feed_processor_store_item.
 * last_poll_ms is array of HU_FEED_COUNT timestamps; updated on successful poll. */
hu_error_t hu_feed_processor_poll(hu_feed_processor_t *proc,
                                  const hu_feed_config_t *config,
                                  uint64_t *last_poll_ms, uint64_t now_ms,
                                  size_t *items_ingested);

#endif /* HU_ENABLE_SQLITE */

#endif
