#ifndef HU_FEEDS_NEWS_H
#define HU_FEEDS_NEWS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
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

hu_error_t hu_news_fetch_rss(hu_allocator_t *alloc,
    const char *feed_url, size_t url_len,
    hu_rss_article_t *articles, size_t cap, size_t *out_count);

#endif /* HU_ENABLE_FEEDS */

#ifdef __cplusplus
}
#endif

#endif
