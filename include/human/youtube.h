#ifndef HU_YOUTUBE_H
#define HU_YOUTUBE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>

typedef struct hu_youtube_result {
    char *video_id;      /* "dQw4w9WgXcQ" */
    char *title;         /* snippet.title */
    char *channel_title; /* snippet.channelTitle */
    char *watch_url;     /* https://www.youtube.com/watch?v=<video_id> */
} hu_youtube_result_t;

/* YouTube Data API v3 search.list (type=video, maxResults=1). Requires an API
 * key. Network-guarded; returns the verified top result. */
hu_error_t hu_youtube_search(hu_allocator_t *alloc, const char *api_key, const char *query,
                             size_t query_len, hu_youtube_result_t *out);

/* Parse a search.list JSON response. Exposed for testing (no network). */
hu_error_t hu_youtube_parse_search_response(hu_allocator_t *alloc, const char *json,
                                            size_t json_len, hu_youtube_result_t *out);

void hu_youtube_result_free(hu_allocator_t *alloc, hu_youtube_result_t *out);

#endif
