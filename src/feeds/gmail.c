/* Gmail feed — fetch recent emails via Gmail REST API with OAuth2. F94. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/gmail.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/feeds/ingest.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GMAIL_FEED_API_BASE  "https://gmail.googleapis.com/gmail/v1/users/me"
#define GMAIL_FEED_TOKEN_URL "https://oauth2.googleapis.com/token"

#if HU_IS_TEST

hu_error_t hu_gmail_feed_fetch(hu_allocator_t *alloc,
    const char *client_id, size_t client_id_len,
    const char *client_secret, size_t client_secret_len,
    const char *refresh_token, size_t refresh_token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)client_id;
    (void)client_id_len;
    (void)client_secret;
    (void)client_secret_len;
    (void)refresh_token;
    (void)refresh_token_len;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "gmail", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "email", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content,
        "New AI framework released: LangGraph 2.0 enables stateful multi-agent workflows",
        sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "gmail", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "email", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content,
        "Weekly AI digest: Claude 4 benchmarks, OpenAI reasoning improvements",
        sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

hu_error_t hu_gmail_feed_fetch(hu_allocator_t *alloc,
    const char *client_id, size_t client_id_len,
    const char *client_secret, size_t client_secret_len,
    const char *refresh_token, size_t refresh_token_len,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    if (!alloc || !items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!client_id || client_id_len == 0 || !client_secret || client_secret_len == 0 ||
        !refresh_token || refresh_token_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if !defined(HU_HTTP_CURL)
    (void)client_id;
    (void)client_id_len;
    (void)client_secret;
    (void)client_secret_len;
    (void)refresh_token;
    (void)refresh_token_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    /* Refresh the access token */
    char body[2048];
    int bn = snprintf(body, sizeof(body),
        "grant_type=refresh_token&client_id=%.*s&client_secret=%.*s&refresh_token=%.*s",
        (int)client_id_len, client_id,
        (int)client_secret_len, client_secret,
        (int)refresh_token_len, refresh_token);
    if (bn <= 0 || (size_t)bn >= sizeof(body))
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t tresp = {0};
    hu_error_t err = hu_http_request(alloc, GMAIL_FEED_TOKEN_URL, "POST",
        "Content-Type: application/x-www-form-urlencoded\nAccept: application/json",
        body, (size_t)bn, &tresp);
    if (err != HU_OK)
        return err;
    if (tresp.status_code < 200 || tresp.status_code >= 300) {
        if (tresp.owned && tresp.body)
            hu_http_response_free(alloc, &tresp);
        return HU_ERR_PROVIDER_AUTH;
    }

    hu_json_value_t *troot = NULL;
    err = hu_json_parse(alloc, tresp.body, tresp.body_len, &troot);
    if (tresp.owned && tresp.body)
        hu_http_response_free(alloc, &tresp);
    if (err != HU_OK || !troot)
        return HU_ERR_PARSE;

    const char *access_token = hu_json_get_string(troot, "access_token");
    if (!access_token || !access_token[0]) {
        hu_json_free(alloc, troot);
        return HU_ERR_PROVIDER_AUTH;
    }

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", access_token);
    hu_json_free(alloc, troot);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    /* List recent messages (up to items_cap, max 20) */
    size_t max_results = items_cap > 20 ? 20 : items_cap;
    char list_url[384];
    int nu = snprintf(list_url, sizeof(list_url),
        "%s/messages?q=is:unread&maxResults=%zu", GMAIL_FEED_API_BASE, max_results);
    if (nu <= 0 || (size_t)nu >= sizeof(list_url))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    err = hu_http_get(alloc, list_url, auth_buf, &resp);
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

    hu_json_value_t *msgs_arr = hu_json_object_get(parsed, "messages");
    if (!msgs_arr || msgs_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < msgs_arr->data.array.len && cnt < items_cap; i++) {
        hu_json_value_t *msg_ref = msgs_arr->data.array.items[i];
        if (!msg_ref || msg_ref->type != HU_JSON_OBJECT)
            continue;
        const char *msg_id = hu_json_get_string(msg_ref, "id");
        if (!msg_id || !msg_id[0])
            continue;

        char get_url[384];
        int ng = snprintf(get_url, sizeof(get_url),
            "%s/messages/%.100s?format=metadata&metadataHeaders=From&metadataHeaders=Subject",
            GMAIL_FEED_API_BASE, msg_id);
        if (ng <= 0 || (size_t)ng >= sizeof(get_url))
            continue;

        hu_http_response_t gresp = {0};
        err = hu_http_get(alloc, get_url, auth_buf, &gresp);
        if (err != HU_OK || !gresp.body || gresp.status_code != 200) {
            if (gresp.owned && gresp.body)
                hu_http_response_free(alloc, &gresp);
            continue;
        }

        hu_json_value_t *msg_full = NULL;
        err = hu_json_parse(alloc, gresp.body, gresp.body_len, &msg_full);
        if (gresp.owned && gresp.body)
            hu_http_response_free(alloc, &gresp);
        if (err != HU_OK || !msg_full)
            continue;

        hu_json_value_t *payload = hu_json_object_get(msg_full, "payload");
        hu_json_value_t *headers = payload ? hu_json_object_get(payload, "headers") : NULL;
        const char *from = NULL;
        const char *subject = NULL;
        if (headers && headers->type == HU_JSON_ARRAY) {
            for (size_t h = 0; h < headers->data.array.len; h++) {
                hu_json_value_t *hdr = headers->data.array.items[h];
                if (!hdr || hdr->type != HU_JSON_OBJECT)
                    continue;
                const char *name = hu_json_get_string(hdr, "name");
                const char *val = hu_json_get_string(hdr, "value");
                if (name && val) {
                    if (strcmp(name, "From") == 0) from = val;
                    else if (strcmp(name, "Subject") == 0) subject = val;
                }
            }
        }

        memset(&items[cnt], 0, sizeof(items[cnt]));
        (void)strncpy(items[cnt].source, "gmail", sizeof(items[cnt].source) - 1);
        (void)strncpy(items[cnt].content_type, "email", sizeof(items[cnt].content_type) - 1);
        snprintf(items[cnt].content, sizeof(items[cnt].content),
            "From: %s | Subject: %s",
            from ? from : "(unknown)", subject ? subject : "(no subject)");
        items[cnt].content_len = strlen(items[cnt].content);
        items[cnt].ingested_at = (int64_t)time(NULL);
        cnt++;
        hu_json_free(alloc, msg_full);
    }

    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#endif /* HU_HTTP_CURL */
}

#endif /* HU_IS_TEST */

#else
typedef int hu_gmail_feed_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
