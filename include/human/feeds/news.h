#ifndef HU_FEEDS_NEWS_H
#define HU_FEEDS_NEWS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HU_ENABLE_FEEDS

typedef struct hu_rss_article {
    char title[256];
    char link[512];
    char description[1024];
    int64_t pub_date;
} hu_rss_article_t;

hu_error_t hu_news_fetch_rss(hu_allocator_t *alloc, const char *feed_url, size_t url_len,
                             hu_rss_article_t *articles, size_t cap, size_t *out_count);

/* Per-URL backoff for feeds that refuse us. 429 and 5xx back off 2 h,
 * doubling per repeat to 24 h; any other status means no backoff (0).
 * hu_news_fetch_rss consults and updates it: a URL in backoff is not
 * requested. Added 2026-09-30: config polls every 30 min and hnrss.org
 * answered 429/502 107 times in a week. */
uint32_t hu_news_backoff_secs(int status, uint32_t prev_secs);
bool hu_news_backoff_active(const char *url, uint64_t now_ms);
void hu_news_backoff_record(const char *url, int status, uint64_t now_ms);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
