/* Phase 7 Google Photos shared albums feed. F86. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/google.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if HU_IS_TEST

static hu_error_t hu_google_photos_fetch_impl(hu_allocator_t *alloc, sqlite3 *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)db;
    (void)access_token;
    (void)token_len;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "google_photos", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "photo", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content, "Beach vacation album", sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    (void)strncpy(items[0].url, "https://photos.google.com/share/1234",
        sizeof(items[0].url) - 1);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "google_photos", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "photo", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content, "Beach vacation album", sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    (void)strncpy(items[1].url, "https://photos.google.com/share/1234",
        sizeof(items[1].url) - 1);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

#define HU_GOOGLE_PHOTOS_SHARED_ALBUMS_URL \
    "https://photoslibrary.googleapis.com/v1/sharedAlbums?pageSize=50"

static hu_error_t hu_google_photos_fetch_impl(hu_allocator_t *alloc, sqlite3 *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)db;
    if (!alloc || !items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!access_token || token_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    char auth_buf[256];
    if (token_len >= sizeof(auth_buf) - 16)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(auth_buf, "Bearer ", 7);
    memcpy(auth_buf + 7, access_token, token_len);
    auth_buf[7 + token_len] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, HU_GOOGLE_PHOTOS_SHARED_ALBUMS_URL,
        auth_buf, &resp);
    if (err != HU_OK)
        return err;
    if (!resp.body || resp.status_code != 200) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    /* Parse sharedAlbums array: {"sharedAlbums":[{"id":"...","title":"..."}]} */
    const char *p = strstr(resp.body, "\"sharedAlbums\"");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;
            size_t n = 0;
            while (n < items_cap) {
                const char *title_start = strstr(p, "\"title\"");
                if (!title_start || title_start - p > 2048)
                    break;
                const char *colon = strchr(title_start, ':');
                if (!colon)
                    break;
                const char *val = colon + 1;
                while (*val == ' ' || *val == '\t')
                    val++;
                if (*val != '"')
                    break;
                val++;
                const char *val_end = strchr(val, '"');
                if (!val_end)
                    break;
                size_t title_len = (size_t)(val_end - val);
                if (title_len >= sizeof(items[n].content))
                    title_len = sizeof(items[n].content) - 1;
                memset(&items[n], 0, sizeof(items[n]));
                (void)strncpy(items[n].source, "google_photos", sizeof(items[n].source) - 1);
                (void)strncpy(items[n].content_type, "photo", sizeof(items[n].content_type) - 1);
                memcpy(items[n].content, val, title_len);
                items[n].content[title_len] = '\0';
                items[n].content_len = title_len;
                items[n].ingested_at = (int64_t)time(NULL);
                const char *id_start = strstr(p, "\"id\"");
                if (id_start && id_start < title_start) {
                    const char *id_colon = strchr(id_start, ':');
                    if (id_colon) {
                        const char *id_val = id_colon + 1;
                        while (*id_val == ' ' || *id_val == '\t')
                            id_val++;
                        if (*id_val == '"') {
                            id_val++;
                            const char *id_end = strchr(id_val, '"');
                            if (id_end) {
                                size_t id_len = (size_t)(id_end - id_val);
                                if (id_len < sizeof(items[n].url) - 40) {
                                    (void)snprintf(items[n].url, sizeof(items[n].url),
                                        "https://photos.google.com/share/%.*s",
                                        (int)id_len, id_val);
                                }
                            }
                        }
                    }
                }
                if (items[n].url[0] == '\0')
                    (void)strncpy(items[n].url, "https://photos.google.com/share/",
                        sizeof(items[n].url) - 1);
                n++;
                p = strchr(val_end, '}');
                if (!p)
                    break;
                p = strchr(p, '{');
                if (!p)
                    break;
            }
            *out_count = n;
        }
    }
    hu_http_response_free(alloc, &resp);
    return HU_OK;
}

#endif /* HU_IS_TEST */

hu_error_t hu_google_photos_fetch(hu_allocator_t *alloc, sqlite3 *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    return hu_google_photos_fetch_impl(alloc, db, access_token, token_len,
        items, items_cap, out_count);
}

#else
typedef int hu_google_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
