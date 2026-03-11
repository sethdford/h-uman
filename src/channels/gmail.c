/*
 * Gmail channel — READ-ONLY ingest via Gmail REST API with OAuth2.
 * Polls unread emails, extracts From/Subject/body, marks as read.
 */
#include "human/channels/gmail.h"
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define GMAIL_API_BASE        "https://gmail.googleapis.com/gmail/v1/users/me"
#define OAUTH_TOKEN_URL       "https://oauth2.googleapis.com/token"
#define GMAIL_SESSION_KEY_MAX 127
#define GMAIL_CONTENT_MAX     4095

typedef struct hu_gmail_ctx {
    hu_allocator_t *alloc;
    char *client_id;
    char *client_secret;
    char *refresh_token;
    char *access_token;
    size_t access_token_len;
    int64_t token_expires_at;
    int poll_interval_sec;
    bool running;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_gmail_ctx_t;

/* ─── Helpers ───────────────────────────────────────────────────────────── */

/* base64url_decode lives in gmail_base64.c for testability */
extern hu_error_t base64url_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                                   size_t *out_len);

#if defined(HU_HTTP_CURL) && !HU_IS_TEST
static int form_encode_char(char *out, size_t cap, size_t *j, unsigned char c) {
    if (*j + 4 > cap)
        return -1;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
        out[(*j)++] = (char)c;
        return 0;
    }
    if (c == ' ') {
        out[(*j)++] = '+';
        return 0;
    }
    out[(*j)++] = '%';
    out[(*j)++] = (char)((c >> 4) < 10 ? '0' + (c >> 4) : 'A' + ((c >> 4) - 10));
    out[(*j)++] = (char)((c & 0x0f) < 10 ? '0' + (c & 0x0f) : 'A' + ((c & 0x0f) - 10));
    return 0;
}

static hu_error_t refresh_access_token(hu_gmail_ctx_t *c) {
    if (!c->client_id || !c->client_secret || !c->refresh_token)
        return HU_ERR_CHANNEL_NOT_CONFIGURED;

    char body[2048];
    size_t j = 0;
#define APPEND_LIT(s)                       \
    do {                                    \
        memcpy(body + j, s, sizeof(s) - 1); \
        j += sizeof(s) - 1;                 \
    } while (0)
    APPEND_LIT("grant_type=refresh_token&client_id=");
    for (const char *p = c->client_id; *p && j < sizeof(body) - 4; p++) {
        if (form_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    APPEND_LIT("&client_secret=");
    for (const char *p = c->client_secret; *p && j < sizeof(body) - 4; p++) {
        if (form_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    APPEND_LIT("&refresh_token=");
#undef APPEND_LIT
    for (const char *p = c->refresh_token; *p && j < sizeof(body) - 4; p++) {
        if (form_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return HU_ERR_INVALID_ARGUMENT;
    }
    body[j] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(
        c->alloc, OAUTH_TOKEN_URL, "POST",
        "Content-Type: application/x-www-form-urlencoded\nAccept: application/json\nUser-Agent: "
        "Human/1.0",
        body, j, &resp);
    if (err != HU_OK) {
        fprintf(stderr, "[gmail] token refresh HTTP error: %d\n", (int)err);
        return err;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        int blen = resp.body_len > 500 ? 500 : (int)resp.body_len;
        fprintf(stderr, "[gmail] token refresh HTTP %ld: %.*s\n", (long)resp.status_code, blen,
                resp.body ? resp.body : "(null)");
        if (resp.owned && resp.body)
            hu_http_response_free(c->alloc, &resp);
        return HU_ERR_PROVIDER_AUTH;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(c->alloc, resp.body, resp.body_len, &root);
    if (resp.owned && resp.body)
        hu_http_response_free(c->alloc, &resp);
    if (err != HU_OK || !root)
        return HU_ERR_PARSE;

    const char *at = hu_json_get_string(root, "access_token");
    double exp_in = hu_json_get_number(root, "expires_in", 3600.0);
    if (!at || !at[0]) {
        fprintf(stderr, "[gmail] token refresh: no access_token in response\n");
        hu_json_free(c->alloc, root);
        return HU_ERR_PROVIDER_AUTH;
    }

    size_t at_len = strlen(at);
    char *at_copy = hu_strndup(c->alloc, at, at_len);
    hu_json_free(c->alloc, root);
    if (!at_copy)
        return HU_ERR_OUT_OF_MEMORY;

    if (c->access_token)
        c->alloc->free(c->alloc->ctx, c->access_token, c->access_token_len + 1);
    c->access_token = at_copy;
    c->access_token_len = at_len;
    c->token_expires_at = (int64_t)time(NULL) + (int64_t)exp_in;
    return HU_OK;
}

static hu_error_t ensure_access_token(hu_gmail_ctx_t *c) {
    int64_t now = (int64_t)time(NULL);
    if (!c->access_token || c->token_expires_at <= now + 300)
        return refresh_access_token(c);
    return HU_OK;
}

const char *get_header_value(hu_json_value_t *headers, const char *name) {
    if (!headers || headers->type != HU_JSON_ARRAY)
        return NULL;
    for (size_t i = 0; i < headers->data.array.len; i++) {
        hu_json_value_t *h = headers->data.array.items[i];
        if (!h || h->type != HU_JSON_OBJECT)
            continue;
        const char *n = hu_json_get_string(h, "name");
        if (n && strcasecmp(n, name) == 0)
            return hu_json_get_string(h, "value");
    }
    return NULL;
}

hu_error_t extract_body_from_payload(hu_allocator_t *alloc, hu_json_value_t *payload, char *out,
                                     size_t out_cap, size_t *out_len) {
    *out_len = 0;
    if (!payload || payload->type != HU_JSON_OBJECT)
        return HU_OK;

    /* Try payload.body.data first (simple messages) */
    hu_json_value_t *body_obj = hu_json_object_get(payload, "body");
    if (body_obj && body_obj->type == HU_JSON_OBJECT) {
        const char *data = hu_json_get_string(body_obj, "data");
        if (data && strlen(data) > 0) {
            size_t dlen = strlen(data);
            char *decoded = (char *)alloc->alloc(alloc->ctx, dlen + 1);
            if (!decoded)
                return HU_ERR_OUT_OF_MEMORY;
            size_t dec_len = 0;
            hu_error_t err = base64url_decode(data, dlen, decoded, dlen + 1, &dec_len);
            if (err != HU_OK) {
                alloc->free(alloc->ctx, decoded, dlen + 1);
                return err;
            }
            size_t copy = dec_len < out_cap - 1 ? dec_len : out_cap - 1;
            memcpy(out, decoded, copy);
            out[copy] = '\0';
            *out_len = copy;
            alloc->free(alloc->ctx, decoded, dlen + 1);
            return HU_OK;
        }
    }

    /* Try payload.parts (multipart) — prefer text/plain */
    hu_json_value_t *parts = hu_json_object_get(payload, "parts");
    if (parts && parts->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < parts->data.array.len; i++) {
            hu_json_value_t *part = parts->data.array.items[i];
            if (!part || part->type != HU_JSON_OBJECT)
                continue;
            const char *mime = hu_json_get_string(part, "mimeType");
            if (!mime ||
                (strcasecmp(mime, "text/plain") != 0 && strcasecmp(mime, "text/html") != 0))
                continue;
            hu_json_value_t *pbody = hu_json_object_get(part, "body");
            if (!pbody || pbody->type != HU_JSON_OBJECT)
                continue;
            const char *data = hu_json_get_string(pbody, "data");
            if (!data || strlen(data) == 0)
                continue;
            size_t dlen = strlen(data);
            char *decoded = (char *)alloc->alloc(alloc->ctx, dlen + 1);
            if (!decoded)
                return HU_ERR_OUT_OF_MEMORY;
            size_t dec_len = 0;
            hu_error_t err = base64url_decode(data, dlen, decoded, dlen + 1, &dec_len);
            if (err != HU_OK) {
                alloc->free(alloc->ctx, decoded, dlen + 1);
                return err;
            }
            /* Strip HTML tags crudely for text/html */
            size_t copy = dec_len < out_cap - 1 ? dec_len : out_cap - 1;
            if (strcasecmp(mime, "text/html") == 0) {
                size_t w = 0;
                bool in_tag = false;
                for (size_t r = 0; r < dec_len && w < out_cap - 1; r++) {
                    if (decoded[r] == '<')
                        in_tag = true;
                    else if (decoded[r] == '>')
                        in_tag = false;
                    else if (!in_tag &&
                             (decoded[r] != '\r' || (r + 1 < dec_len && decoded[r + 1] != '\n')))
                        out[w++] = decoded[r] == '\n' ? ' ' : decoded[r];
                }
                out[w] = '\0';
                copy = w;
            } else {
                memcpy(out, decoded, copy);
                out[copy] = '\0';
            }
            *out_len = copy;
            alloc->free(alloc->ctx, decoded, dlen + 1);
            return HU_OK;
        }
    }
    return HU_OK;
}
#endif

/* ─── Vtable ────────────────────────────────────────────────────────────── */

static hu_error_t gmail_start(void *ctx) {
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void gmail_stop(void *ctx) {
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ctx;
    if (c)
        c->running = false;
}

#if HU_IS_TEST
static __attribute__((unused)) hu_error_t gmail_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ctx;
    size_t len = message_len > 4095 ? 4095 : message_len;
    if (message && len > 0)
        memcpy(c->last_message, message, len);
    c->last_message[len] = '\0';
    c->last_message_len = len;
    return HU_ERR_NOT_SUPPORTED;
}
#endif

static const char *gmail_name(void *ctx) {
    (void)ctx;
    return "gmail";
}

static bool gmail_health_check(void *ctx) {
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ctx;
    if (!c)
        return false;
    return c->access_token != NULL && c->access_token_len > 0;
}

static hu_error_t gmail_load_conversation_history(void *ctx, hu_allocator_t *alloc,
                                                  const char *contact_id, size_t contact_id_len,
                                                  size_t limit, hu_channel_history_entry_t **out,
                                                  size_t *out_count) {
    (void)ctx;
    if (!alloc || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

#if HU_IS_TEST || !defined(HU_HTTP_CURL)
    (void)contact_id_len;
    (void)limit;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = ensure_access_token(c);
    if (err != HU_OK)
        return err;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", c->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    if (limit > 10)
        limit = 10;

    /* Search for messages from/to this contact */
    char query_buf[256];
    char contact_buf[128];
    size_t clen =
        contact_id_len < sizeof(contact_buf) - 1 ? contact_id_len : sizeof(contact_buf) - 1;
    memcpy(contact_buf, contact_id, clen);
    contact_buf[clen] = '\0';

    /* URL-encode spaces in the query by using {from:X OR to:X} */
    size_t qj = 0;
    const char *prefix = "from:";
    for (const char *p = prefix; *p; p++)
        query_buf[qj++] = *p;
    for (size_t i = 0; i < clen && qj < sizeof(query_buf) - 20; i++)
        query_buf[qj++] = contact_buf[i];
    const char *mid = "+OR+to:";
    for (const char *p = mid; *p; p++)
        query_buf[qj++] = *p;
    for (size_t i = 0; i < clen && qj < sizeof(query_buf) - 2; i++)
        query_buf[qj++] = contact_buf[i];
    query_buf[qj] = '\0';

    char list_url[512];
    int nu = snprintf(list_url, sizeof(list_url), "%s/messages?q=%s&maxResults=%zu", GMAIL_API_BASE,
                      query_buf, limit);
    if (nu <= 0 || (size_t)nu >= sizeof(list_url))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    err = hu_http_get(alloc, list_url, auth_buf, &resp);
    if (err != HU_OK || !resp.body || resp.status_code != 200) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err != HU_OK ? err : HU_ERR_INTERNAL;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;

    hu_json_value_t *messages_arr = hu_json_object_get(parsed, "messages");
    if (!messages_arr || messages_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t arr_len = messages_arr->data.array.len;
    if (arr_len > limit)
        arr_len = limit;

    hu_channel_history_entry_t *entries =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, arr_len * sizeof(*entries));
    if (!entries) {
        hu_json_free(alloc, parsed);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, arr_len * sizeof(*entries));
    size_t count = 0;

    for (size_t i = 0; i < arr_len; i++) {
        hu_json_value_t *msg_ref = messages_arr->data.array.items[i];
        if (!msg_ref || msg_ref->type != HU_JSON_OBJECT)
            continue;
        const char *msg_id = hu_json_get_string(msg_ref, "id");
        if (!msg_id)
            continue;

        char get_url[384];
        int ng = snprintf(get_url, sizeof(get_url),
                          "%s/messages/"
                          "%.100s?format=metadata&metadataHeaders=From&metadataHeaders=Subject&"
                          "metadataHeaders=Date",
                          GMAIL_API_BASE, msg_id);
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
        const char *from = get_header_value(headers, "From");
        const char *subject = get_header_value(headers, "Subject");
        const char *date = get_header_value(headers, "Date");

        /* Determine from_me by checking if "from" contains our own address */
        bool from_me = false;
        if (from) {
            /* Simple heuristic: if "from" doesn't contain the contact_id, it's from us */
            if (!strstr(from, contact_buf))
                from_me = true;
        }

        entries[count].from_me = from_me;
        snprintf(entries[count].text, sizeof(entries[0].text), "[Email] %s: %s",
                 from_me ? "You" : (from ? from : "?"), subject ? subject : "(no subject)");
        if (date) {
            size_t dlen = strlen(date);
            if (dlen >= sizeof(entries[0].timestamp))
                dlen = sizeof(entries[0].timestamp) - 1;
            memcpy(entries[count].timestamp, date, dlen);
            entries[count].timestamp[dlen] = '\0';
        }
        count++;
        hu_json_free(alloc, msg_full);
    }

    hu_json_free(alloc, parsed);

    /* Reverse to chronological order (API returns newest first) */
    for (size_t i = 0; i < count / 2; i++) {
        hu_channel_history_entry_t tmp = entries[i];
        entries[i] = entries[count - 1 - i];
        entries[count - 1 - i] = tmp;
    }

    *out = entries;
    *out_count = count;
    return HU_OK;
#endif
}

static const hu_channel_vtable_t gmail_vtable = {
    .start = gmail_start,
    .stop = gmail_stop,
    .send = NULL, /* read-only: daemon skips agent turn when send is NULL */
    .name = gmail_name,
    .health_check = gmail_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
    .load_conversation_history = gmail_load_conversation_history,
};

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_gmail_create(hu_allocator_t *alloc, const char *client_id, size_t client_id_len,
                           const char *client_secret, size_t client_secret_len,
                           const char *refresh_token, size_t refresh_token_len,
                           int poll_interval_sec, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->poll_interval_sec = poll_interval_sec > 0 ? poll_interval_sec : 60;

    if (client_id && client_id_len > 0) {
        c->client_id = hu_strndup(alloc, client_id, client_id_len);
        if (!c->client_id) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    if (client_secret && client_secret_len > 0) {
        c->client_secret = hu_strndup(alloc, client_secret, client_secret_len);
        if (!c->client_secret) {
            if (c->client_id)
                alloc->free(alloc->ctx, c->client_id, strlen(c->client_id) + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    if (refresh_token && refresh_token_len > 0) {
        c->refresh_token = hu_strndup(alloc, refresh_token, refresh_token_len);
        if (!c->refresh_token) {
            if (c->client_id)
                alloc->free(alloc->ctx, c->client_id, strlen(c->client_id) + 1);
            if (c->client_secret)
                alloc->free(alloc->ctx, c->client_secret, strlen(c->client_secret) + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }

    out->ctx = c;
    out->vtable = &gmail_vtable;
    return HU_OK;
}

void hu_gmail_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (a) {
            if (c->client_id)
                a->free(a->ctx, c->client_id, strlen(c->client_id) + 1);
            if (c->client_secret)
                a->free(a->ctx, c->client_secret, strlen(c->client_secret) + 1);
            if (c->refresh_token)
                a->free(a->ctx, c->refresh_token, strlen(c->refresh_token) + 1);
            if (c->access_token)
                a->free(a->ctx, c->access_token, c->access_token_len + 1);
            a->free(a->ctx, c, sizeof(*c));
        }
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

bool hu_gmail_is_configured(hu_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ch->ctx;
    return c->client_id != NULL && c->client_secret != NULL && c->refresh_token != NULL;
}

/* ─── Poll ──────────────────────────────────────────────────────────────── */

hu_error_t hu_gmail_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)channel_ctx;
    if (!c || !alloc || !msgs || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if HU_IS_TEST
    if (c->mock_count > 0) {
        size_t n = c->mock_count < max_msgs ? c->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, c->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, c->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        c->mock_count = 0;
        return HU_OK;
    }
    return HU_OK;
#elif !defined(HU_HTTP_CURL)
    (void)max_msgs;
    return HU_ERR_NOT_SUPPORTED;
#else
    if (!c->client_id || !c->client_secret || !c->refresh_token)
        return HU_OK;

    hu_error_t err = ensure_access_token(c);
    if (err != HU_OK)
        return err;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", c->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return HU_ERR_INTERNAL;

    /* GET messages?q=is:unread&maxResults=10 */
    char list_url[384];
    int nu = snprintf(list_url, sizeof(list_url), "%s/messages?q=is:unread&maxResults=10",
                      GMAIL_API_BASE);
    if (nu <= 0 || (size_t)nu >= sizeof(list_url))
        return HU_ERR_INTERNAL;

    hu_http_response_t resp = {0};
    err = hu_http_get(alloc, list_url, auth_buf, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err;
    }
    if (!resp.body || resp.body_len == 0 || resp.status_code != 200) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_OK;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed)
        return HU_OK;

    hu_json_value_t *messages_arr = hu_json_object_get(parsed, "messages");
    if (!messages_arr || messages_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, parsed);
        return HU_OK;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < messages_arr->data.array.len && cnt < max_msgs; i++) {
        hu_json_value_t *msg_ref = messages_arr->data.array.items[i];
        if (!msg_ref || msg_ref->type != HU_JSON_OBJECT)
            continue;
        const char *msg_id = hu_json_get_string(msg_ref, "id");
        if (!msg_id || !msg_id[0])
            continue;

        /* GET full message */
        char get_url[384];
        int ng = snprintf(get_url, sizeof(get_url), "%s/messages/%.100s?format=full",
                          GMAIL_API_BASE, msg_id);
        if (ng <= 0 || (size_t)ng >= sizeof(get_url))
            continue;

        hu_http_response_t gresp = {0};
        err = hu_http_get(alloc, get_url, auth_buf, &gresp);
        if (err != HU_OK || !gresp.body || gresp.body_len == 0 || gresp.status_code != 200) {
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
        if (!payload || payload->type != HU_JSON_OBJECT) {
            hu_json_free(alloc, msg_full);
            continue;
        }

        hu_json_value_t *headers = hu_json_object_get(payload, "headers");
        const char *from = get_header_value(headers, "From");
        const char *subject = get_header_value(headers, "Subject");
        if (!from)
            from = "";
        if (!subject)
            subject = "";

        char body_buf[GMAIL_CONTENT_MAX + 1];
        size_t body_len = 0;
        err = extract_body_from_payload(alloc, payload, body_buf, sizeof(body_buf), &body_len);
        if (err != HU_OK) {
            hu_json_free(alloc, msg_full);
            continue;
        }
        body_buf[body_len] = '\0';

        /* Format: "Email from {from} | Subject: {subject}\n{body}" */
        char content_buf[sizeof(msgs[0].content)];
        int nc = snprintf(content_buf, sizeof(content_buf), "Email from %s | Subject: %s\n%s", from,
                          subject, body_buf);
        if (nc <= 0 || (size_t)nc >= sizeof(content_buf))
            nc = (int)(sizeof(content_buf) - 1);
        size_t content_len = (size_t)nc;

        /* session_key = sender email (from) */
        size_t from_len = strlen(from);
        if (from_len > GMAIL_SESSION_KEY_MAX)
            from_len = GMAIL_SESSION_KEY_MAX;
        memcpy(msgs[cnt].session_key, from, from_len);
        msgs[cnt].session_key[from_len] = '\0';

        if (content_len > GMAIL_CONTENT_MAX)
            content_len = GMAIL_CONTENT_MAX;
        memcpy(msgs[cnt].content, content_buf, content_len);
        msgs[cnt].content[content_len] = '\0';
        cnt++;

        hu_json_free(alloc, msg_full);

        /* Mark as read: POST modify with removeLabelIds: ["UNREAD"] */
        char mod_url[384];
        int nm =
            snprintf(mod_url, sizeof(mod_url), "%s/messages/%.100s/modify", GMAIL_API_BASE, msg_id);
        if (nm > 0 && (size_t)nm < sizeof(mod_url)) {
            const char *mod_body = "{\"removeLabelIds\":[\"UNREAD\"]}";
            hu_http_response_t mresp = {0};
            hu_error_t mod_err = hu_http_post_json(alloc, mod_url, auth_buf, mod_body, 29, &mresp);
            if (mresp.owned && mresp.body)
                hu_http_response_free(alloc, &mresp);
            if (mod_err != HU_OK) {
                hu_json_free(alloc, parsed);
                *out_count = cnt;
                return mod_err;
            }
        }
    }

    hu_json_free(alloc, parsed);
    *out_count = cnt;
    return HU_OK;
#endif
}

#if HU_IS_TEST
hu_error_t hu_gmail_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                     size_t session_key_len, const char *content,
                                     size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return HU_OK;
}
const char *hu_gmail_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_gmail_ctx_t *c = (hu_gmail_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
