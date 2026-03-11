/* Phase 7 social (Facebook/Instagram) feed ingestion. F83/F84. */
#ifdef HU_ENABLE_SOCIAL

#include "human/feeds/social.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if HU_IS_TEST
static hu_error_t hu_social_fetch_facebook_impl(hu_allocator_t *alloc, void *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)db;
    (void)access_token;
    (void)token_len;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "facebook", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "post", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content, "Mock Facebook post 1", sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "facebook", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "post", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content, "Mock Facebook post 2", sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

static hu_error_t hu_social_fetch_instagram_impl(hu_allocator_t *alloc, void *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)db;
    (void)access_token;
    (void)token_len;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "instagram", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "media", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content, "Mock Instagram media 1", sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "instagram", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "media", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content, "Mock Instagram media 2", sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else
/* Non-test: Graph API via hu_http_get. Rate limit: 200 calls/hour. */
static hu_error_t hu_social_fetch_facebook_impl(hu_allocator_t *alloc, void *db,
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
    char url[384];
    (void)snprintf(url, sizeof(url),
        "https://graph.facebook.com/me/feed?fields=id,message,created_time&limit=%zu",
        items_cap > 25 ? 25u : items_cap);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, auth_buf, &resp);
    if (err != HU_OK)
        return err;
    if (!resp.body || resp.status_code != 200) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }
    /* Minimal JSON parse: look for "data":[{"id":...,"message":...}] */
    const char *p = strstr(resp.body, "\"data\"");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;
            size_t n = 0;
            while (n < items_cap) {
                const char *id_start = strstr(p, "\"id\"");
                if (!id_start || id_start - p > 1024)
                    break;
                const char *msg_start = strstr(id_start, "\"message\"");
                if (!msg_start || msg_start - id_start > 512)
                    break;
                const char *msg_val = strchr(msg_start, ':');
                if (!msg_val)
                    break;
                msg_val++;
                while (*msg_val == ' ' || *msg_val == '\t')
                    msg_val++;
                if (*msg_val != '"')
                    break;
                msg_val++;
                const char *msg_end = strchr(msg_val, '"');
                if (!msg_end)
                    break;
                size_t msg_len = (size_t)(msg_end - msg_val);
                if (msg_len >= sizeof(items[n].content))
                    msg_len = sizeof(items[n].content) - 1;
                memset(&items[n], 0, sizeof(items[n]));
                (void)strncpy(items[n].source, "facebook", sizeof(items[n].source) - 1);
                (void)strncpy(items[n].content_type, "post", sizeof(items[n].content_type) - 1);
                memcpy(items[n].content, msg_val, msg_len);
                items[n].content[msg_len] = '\0';
                items[n].content_len = msg_len;
                items[n].ingested_at = (int64_t)time(NULL);
                n++;
                p = msg_end + 1;
                if (strchr(p, '}') == NULL)
                    break;
            }
            *out_count = n;
        }
    }
    hu_http_response_free(alloc, &resp);
    return HU_OK;
}

static hu_error_t hu_social_fetch_instagram_impl(hu_allocator_t *alloc, void *db,
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
    char url[384];
    (void)snprintf(url, sizeof(url),
        "https://graph.instagram.com/me/media?fields=id,caption&limit=%zu",
        items_cap > 25 ? 25u : items_cap);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, auth_buf, &resp);
    if (err != HU_OK)
        return err;
    if (!resp.body || resp.status_code != 200) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }
    const char *p = strstr(resp.body, "\"data\"");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;
            size_t n = 0;
            while (n < items_cap) {
                const char *id_start = strstr(p, "\"id\"");
                if (!id_start || id_start - p > 1024)
                    break;
                const char *cap_start = strstr(id_start, "\"caption\"");
                const char *msg_val;
                size_t msg_len;
                if (cap_start && cap_start - id_start < 256) {
                    msg_val = strchr(cap_start, ':');
                    if (!msg_val)
                        break;
                    msg_val++;
                    while (*msg_val == ' ' || *msg_val == '\t')
                        msg_val++;
                    if (*msg_val != '"')
                        break;
                    msg_val++;
                    const char *msg_end = strchr(msg_val, '"');
                    if (!msg_end)
                        break;
                    msg_len = (size_t)(msg_end - msg_val);
                } else {
                    msg_val = "Instagram media";
                    msg_len = 14;
                }
                if (msg_len >= sizeof(items[n].content))
                    msg_len = sizeof(items[n].content) - 1;
                memset(&items[n], 0, sizeof(items[n]));
                (void)strncpy(items[n].source, "instagram", sizeof(items[n].source) - 1);
                (void)strncpy(items[n].content_type, "media", sizeof(items[n].content_type) - 1);
                memcpy(items[n].content, msg_val, msg_len);
                items[n].content[msg_len] = '\0';
                items[n].content_len = msg_len;
                items[n].ingested_at = (int64_t)time(NULL);
                n++;
                p = strchr(id_start, '}');
                if (!p)
                    break;
                p++;
            }
            *out_count = n;
        }
    }
    hu_http_response_free(alloc, &resp);
    return HU_OK;
}

#endif /* HU_IS_TEST */

hu_error_t hu_social_fetch_facebook(hu_allocator_t *alloc, void *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    return hu_social_fetch_facebook_impl(alloc, db, access_token, token_len,
        items, items_cap, out_count);
}

hu_error_t hu_social_fetch_instagram(hu_allocator_t *alloc, void *db,
    const char *access_token, size_t token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    return hu_social_fetch_instagram_impl(alloc, db, access_token, token_len,
        items, items_cap, out_count);
}

#else
/* Stub when HU_ENABLE_SOCIAL is off — avoids empty translation unit warning */
typedef int hu_social_stub_avoid_empty_tu;
#endif /* HU_ENABLE_SOCIAL */
