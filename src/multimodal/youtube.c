#include "human/youtube.h"
#include "human/core/json.h"
#include "human/music.h"
#include <stdio.h>
#include <string.h>

static char *yt_dup(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *d = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (d) {
        memcpy(d, s, n + 1);
    }
    return d;
}

hu_error_t hu_youtube_parse_search_response(hu_allocator_t *alloc, const char *json,
                                            size_t json_len, hu_youtube_result_t *out) {
    if (!alloc || !json || json_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK)
        return err;

    hu_json_value_t *items = hu_json_object_get(root, "items");
    if (!items || items->type != HU_JSON_ARRAY || items->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    hu_json_value_t *first = items->data.array.items[0];
    if (!first || first->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    hu_json_value_t *id = hu_json_object_get(first, "id");
    hu_json_value_t *sn = hu_json_object_get(first, "snippet");

    const char *vid = id ? hu_json_get_string(id, "videoId") : NULL;
    if (!vid || !*vid) {
        hu_json_free(alloc, root);
        return HU_ERR_NOT_FOUND;
    }

    out->video_id = yt_dup(alloc, vid);
    out->title = yt_dup(alloc, sn ? hu_json_get_string(sn, "title") : NULL);
    out->channel_title = yt_dup(alloc, sn ? hu_json_get_string(sn, "channelTitle") : NULL);

    char url[128];
    int n = snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", vid);
    out->watch_url = (n > 0 && (size_t)n < sizeof(url)) ? yt_dup(alloc, url) : NULL;

    hu_json_free(alloc, root);

    if (!out->video_id || !out->watch_url) {
        hu_youtube_result_free(alloc, out);
        return HU_ERR_PARSE;
    }

    return HU_OK;
}

void hu_youtube_result_free(hu_allocator_t *alloc, hu_youtube_result_t *out) {
    if (!alloc || !out)
        return;
    if (out->video_id) {
        alloc->free(alloc->ctx, out->video_id, strlen(out->video_id) + 1);
    }
    if (out->title) {
        alloc->free(alloc->ctx, out->title, strlen(out->title) + 1);
    }
    if (out->channel_title) {
        alloc->free(alloc->ctx, out->channel_title, strlen(out->channel_title) + 1);
    }
    if (out->watch_url) {
        alloc->free(alloc->ctx, out->watch_url, strlen(out->watch_url) + 1);
    }
    memset(out, 0, sizeof(*out));
}

/* snprintf result as a length, or 0 with out emptied when it was cut off. */
static size_t yt_fit(char *out, size_t cap, int n) {
    if (n > 0 && (size_t)n < cap)
        return (size_t)n;
    out[0] = '\0';
    return 0;
}

size_t hu_youtube_search_url(char *out, size_t cap, const char *api_key, const char *query,
                             size_t query_len, bool shorts) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!api_key || !*api_key || !query || query_len == 0)
        return 0;
    char enc[512];
    if (hu_music_url_encode_query(query, query_len, enc, sizeof(enc)) == 0)
        return 0;
    return yt_fit(out, cap,
                  snprintf(out, cap,
                           "https://www.googleapis.com/youtube/v3/search?part=snippet&type=video"
                           "&maxResults=1%s&q=%s&key=%s",
                           shorts ? "&videoDuration=short" : "", enc, api_key));
}

size_t hu_youtube_share_url(char *out, size_t cap, const char *video_id, bool shorts) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!video_id || !*video_id)
        return 0;
    return yt_fit(out, cap,
                  snprintf(out, cap,
                           shorts ? "https://www.youtube.com/shorts/%s"
                                  : "https://www.youtube.com/watch?v=%s",
                           video_id));
}

#if !defined(HU_IS_TEST) && defined(HU_HTTP_CURL)
#include "human/core/http.h"
#include "human/music.h"

hu_error_t hu_youtube_search_ex(hu_allocator_t *alloc, const char *api_key, const char *query,
                                size_t query_len, bool shorts, hu_youtube_result_t *out) {
    if (!alloc || !api_key || !*api_key || !query || query_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    char url[1024];
    if (hu_youtube_search_url(url, sizeof(url), api_key, query, query_len, shorts) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }

    if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    err = hu_youtube_parse_search_response(alloc, resp.body, resp.body_len, out);
    hu_http_response_free(alloc, &resp);
    if (err == HU_OK && shorts && out->video_id && out->watch_url) {
        char su[128];
        size_t sn = hu_youtube_share_url(su, sizeof(su), out->video_id, true);
        char *dup = sn ? alloc->alloc(alloc->ctx, sn + 1) : NULL;
        if (dup) {
            memcpy(dup, su, sn + 1);
            alloc->free(alloc->ctx, out->watch_url, strlen(out->watch_url) + 1);
            out->watch_url = dup;
        }
    }
    return err;
}

hu_error_t hu_youtube_search(hu_allocator_t *alloc, const char *api_key, const char *query,
                             size_t query_len, hu_youtube_result_t *out) {
    return hu_youtube_search_ex(alloc, api_key, query, query_len, false, out);
}
#else
hu_error_t hu_youtube_search_ex(hu_allocator_t *alloc, const char *api_key, const char *query,
                                size_t query_len, bool shorts, hu_youtube_result_t *out) {
    (void)shorts;
    return hu_youtube_search(alloc, api_key, query, query_len, out);
}

hu_error_t hu_youtube_search(hu_allocator_t *alloc, const char *api_key, const char *query,
                             size_t query_len, hu_youtube_result_t *out) {
    (void)alloc;
    (void)api_key;
    (void)query;
    (void)query_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}
#endif
