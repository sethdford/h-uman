/* Phase 7 Spotify/Music awareness feed. F89. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/music.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if HU_IS_TEST

static hu_error_t hu_spotify_fetch_recent_impl(hu_allocator_t *alloc, sqlite3 *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)db;
    (void)access_token;
    (void)token_len;
    if (!items || !out_count || items_cap < 3)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 3);
    (void)strncpy(items[0].source, "spotify", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "track", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content, "Bohemian Rhapsody by Queen",
        sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "spotify", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "track", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content, "Blinding Lights by The Weeknd",
        sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[2].source, "spotify", sizeof(items[2].source) - 1);
    (void)strncpy(items[2].content_type, "track", sizeof(items[2].content_type) - 1);
    (void)strncpy(items[2].content, "Levitating by Dua Lipa",
        sizeof(items[2].content) - 1);
    items[2].content_len = strlen(items[2].content);
    items[2].ingested_at = (int64_t)time(NULL);
    *out_count = 3;
    return HU_OK;
}

#else

#define HU_SPOTIFY_RECENTLY_PLAYED_URL \
    "https://api.spotify.com/v1/me/player/recently-played?limit=20"

static hu_error_t hu_spotify_fetch_recent_impl(hu_allocator_t *alloc, sqlite3 *db,
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
    hu_error_t err = hu_http_get(alloc, HU_SPOTIFY_RECENTLY_PLAYED_URL,
        auth_buf, &resp);
    if (err != HU_OK)
        return err;
    if (!resp.body || resp.status_code != 200) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    /* Parse items array: {"items":[{"track":{"name":"...","artists":[{"name":"..."}]}}]} */
    const char *p = strstr(resp.body, "\"items\"");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;
            size_t n = 0;
            while (n < items_cap) {
                memset(&items[n], 0, sizeof(items[n]));
                const char *track_start = strstr(p, "\"track\"");
                if (!track_start || track_start - p > 4096)
                    break;
                const char *name_start = strstr(track_start, "\"name\"");
                if (!name_start || name_start - track_start > 1024)
                    break;
                const char *name_colon = strchr(name_start, ':');
                if (!name_colon)
                    break;
                const char *name_val = name_colon + 1;
                while (*name_val == ' ' || *name_val == '\t')
                    name_val++;
                if (*name_val != '"')
                    break;
                name_val++;
                const char *name_end = strchr(name_val, '"');
                if (!name_end)
                    break;
                size_t name_len = (size_t)(name_end - name_val);

                const char *artists_start = strstr(track_start, "\"artists\"");
                const char *artist_name = artists_start ?
                    strstr(artists_start, "\"name\"") : NULL;
                size_t artist_len = 0;
                const char *artist_val = NULL;
                if (artist_name) {
                    const char *ac = strchr(artist_name, ':');
                    if (ac) {
                        artist_val = ac + 1;
                        while (*artist_val == ' ' || *artist_val == '\t')
                            artist_val++;
                        if (*artist_val == '"') {
                            artist_val++;
                            const char *ae = strchr(artist_val, '"');
                            if (ae)
                                artist_len = (size_t)(ae - artist_val);
                        }
                    }
                }

                size_t content_cap = sizeof(items[n].content);
                int written;
                if (artist_val && artist_len > 0) {
                    size_t by_len = 4; /* " by " */
                    if (name_len + by_len + artist_len < content_cap)
                        written = snprintf(items[n].content, content_cap,
                            "%.*s by %.*s", (int)name_len, name_val,
                            (int)artist_len, artist_val);
                    else
                        written = snprintf(items[n].content, content_cap,
                            "%.*s", (int)(content_cap - 1), name_val);
                } else {
                    written = snprintf(items[n].content, content_cap,
                        "%.*s", (int)(name_len < content_cap ? name_len : content_cap - 1),
                        name_val);
                }
                if (written > 0 && (size_t)written < content_cap)
                    items[n].content_len = (size_t)written;
                else if (written > 0)
                    items[n].content_len = content_cap - 1;

                (void)strncpy(items[n].source, "spotify", sizeof(items[n].source) - 1);
                (void)strncpy(items[n].content_type, "track",
                    sizeof(items[n].content_type) - 1);
                items[n].ingested_at = (int64_t)time(NULL);
                n++;
                p = strchr(name_end, '}');
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

hu_error_t hu_spotify_fetch_recent(hu_allocator_t *alloc, sqlite3 *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    return hu_spotify_fetch_recent_impl(alloc, db, access_token, token_len,
        items, items_cap, out_count);
}

#else
typedef int hu_music_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
