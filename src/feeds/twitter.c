/* Twitter/X feed — fetch home timeline via API v2. F96. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/twitter.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/feeds/ingest.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TWITTER_API_BASE "https://api.x.com/2"

#if HU_IS_TEST

hu_error_t hu_twitter_feed_fetch(hu_allocator_t *alloc,
    const char *bearer_token, size_t bearer_token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)bearer_token;
    (void)bearer_token_len;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "twitter", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "tweet", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content,
        "Just released: open-source agent framework with tool-use and memory. "
        "Benchmarks show 3x improvement over previous SOTA.",
        sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    (void)strncpy(items[0].url, "https://x.com/user/status/123", sizeof(items[0].url) - 1);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "twitter", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "tweet", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content,
        "Interesting paper on sub-30ms inference for small language models. "
        "Fits perfectly in embedded devices.",
        sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    (void)strncpy(items[1].url, "https://x.com/user/status/456", sizeof(items[1].url) - 1);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

hu_error_t hu_twitter_feed_fetch(hu_allocator_t *alloc,
    const char *bearer_token, size_t bearer_token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    if (!alloc || !items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!bearer_token || bearer_token_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if !defined(HU_HTTP_CURL)
    (void)bearer_token;
    (void)bearer_token_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s",
        (int)bearer_token_len, bearer_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INVALID_ARGUMENT;

    size_t max_results = items_cap > 25 ? 25 : items_cap;
    char url[384];
    int nu = snprintf(url, sizeof(url),
        "%s/users/me/timelines/reverse_chronological?max_results=%zu"
        "&tweet.fields=text,created_at,author_id",
        TWITTER_API_BASE, max_results);
    if (nu <= 0 || (size_t)nu >= sizeof(url))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, auth_buf, &resp);
    if (err != HU_OK || !resp.body || resp.status_code != 200) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err != HU_OK ? err : HU_ERR_IO;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;

    hu_json_value_t *data = hu_json_object_get(parsed, "data");
    if (!data || data->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < data->data.array.len && cnt < items_cap; i++) {
        hu_json_value_t *tweet = data->data.array.items[i];
        if (!tweet || tweet->type != HU_JSON_OBJECT)
            continue;
        const char *text = hu_json_get_string(tweet, "text");
        const char *tid = hu_json_get_string(tweet, "id");
        const char *author = hu_json_get_string(tweet, "author_id");
        if (!text || !text[0])
            continue;

        memset(&items[cnt], 0, sizeof(items[cnt]));
        (void)strncpy(items[cnt].source, "twitter", sizeof(items[cnt].source) - 1);
        (void)strncpy(items[cnt].content_type, "tweet", sizeof(items[cnt].content_type) - 1);
        snprintf(items[cnt].content, sizeof(items[cnt].content), "%s", text);
        items[cnt].content_len = strlen(items[cnt].content);
        if (tid && author)
            snprintf(items[cnt].url, sizeof(items[cnt].url),
                "https://x.com/%s/status/%s", author, tid);
        items[cnt].ingested_at = (int64_t)time(NULL);
        cnt++;
    }

    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#endif /* HU_HTTP_CURL */
}

#endif /* HU_IS_TEST */

#else
typedef int hu_twitter_feed_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
