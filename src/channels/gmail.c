/*
 * Gmail channel — READ-ONLY ingest via Gmail REST API with OAuth2.
 * Polls unread emails, extracts From/Subject/body, marks as read.
 */
#include "seaclaw/channels/gmail.h"
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define GMAIL_API_BASE        "https://gmail.googleapis.com/gmail/v1/users/me"
#define OAUTH_TOKEN_URL       "https://oauth2.googleapis.com/token"
#define GMAIL_SESSION_KEY_MAX 127
#define GMAIL_CONTENT_MAX     4095

typedef struct sc_gmail_ctx {
    sc_allocator_t *alloc;
    char *client_id;
    char *client_secret;
    char *refresh_token;
    char *access_token;
    size_t access_token_len;
    int64_t token_expires_at;
    int poll_interval_sec;
    bool running;
} sc_gmail_ctx_t;

/* ─── Helpers ───────────────────────────────────────────────────────────── */

/* base64url_decode lives in gmail_base64.c for testability */
extern sc_error_t base64url_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                                   size_t *out_len);

#if defined(SC_HTTP_CURL) && !SC_IS_TEST
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

static sc_error_t refresh_access_token(sc_gmail_ctx_t *c) {
    if (!c->client_id || !c->client_secret || !c->refresh_token)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;

    char body[2048];
    size_t j = 0;
    memcpy(body + j, "grant_type=refresh_token&client_id=", 34);
    j += 34;
    for (const char *p = c->client_id; *p && j < sizeof(body) - 4; p++) {
        if (form_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return SC_ERR_INVALID_ARGUMENT;
    }
    memcpy(body + j, "&client_secret=", 16);
    j += 16;
    for (const char *p = c->client_secret; *p && j < sizeof(body) - 4; p++) {
        if (form_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return SC_ERR_INVALID_ARGUMENT;
    }
    memcpy(body + j, "&refresh_token=", 16);
    j += 16;
    for (const char *p = c->refresh_token; *p && j < sizeof(body) - 4; p++) {
        if (form_encode_char(body, sizeof(body), &j, (unsigned char)*p) != 0)
            return SC_ERR_INVALID_ARGUMENT;
    }
    body[j] = '\0';

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_request(
        c->alloc, OAUTH_TOKEN_URL, "POST",
        "Content-Type: application/x-www-form-urlencoded\nAccept: application/json\nUser-Agent: "
        "SeaClaw/1.0",
        body, j, &resp);
    if (err != SC_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        if (resp.owned && resp.body)
            sc_http_response_free(c->alloc, &resp);
        return SC_ERR_PROVIDER_AUTH;
    }

    sc_json_value_t *root = NULL;
    err = sc_json_parse(c->alloc, resp.body, resp.body_len, &root);
    if (resp.owned && resp.body)
        sc_http_response_free(c->alloc, &resp);
    if (err != SC_OK || !root)
        return SC_ERR_PARSE;

    const char *at = sc_json_get_string(root, "access_token");
    double exp_in = sc_json_get_number(root, "expires_in", 3600.0);
    if (!at || !at[0]) {
        sc_json_free(c->alloc, root);
        return SC_ERR_PROVIDER_AUTH;
    }

    size_t at_len = strlen(at);
    char *at_copy = sc_strndup(c->alloc, at, at_len);
    sc_json_free(c->alloc, root);
    if (!at_copy)
        return SC_ERR_OUT_OF_MEMORY;

    if (c->access_token)
        c->alloc->free(c->alloc->ctx, c->access_token, c->access_token_len + 1);
    c->access_token = at_copy;
    c->access_token_len = at_len;
    c->token_expires_at = (int64_t)time(NULL) + (int64_t)exp_in;
    return SC_OK;
}

static sc_error_t ensure_access_token(sc_gmail_ctx_t *c) {
    int64_t now = (int64_t)time(NULL);
    if (!c->access_token || c->token_expires_at <= now + 300)
        return refresh_access_token(c);
    return SC_OK;
}

const char *get_header_value(sc_json_value_t *headers, const char *name) {
    if (!headers || headers->type != SC_JSON_ARRAY)
        return NULL;
    for (size_t i = 0; i < headers->data.array.len; i++) {
        sc_json_value_t *h = headers->data.array.items[i];
        if (!h || h->type != SC_JSON_OBJECT)
            continue;
        const char *n = sc_json_get_string(h, "name");
        if (n && strcasecmp(n, name) == 0)
            return sc_json_get_string(h, "value");
    }
    return NULL;
}

sc_error_t extract_body_from_payload(sc_allocator_t *alloc, sc_json_value_t *payload, char *out,
                                     size_t out_cap, size_t *out_len) {
    *out_len = 0;
    if (!payload || payload->type != SC_JSON_OBJECT)
        return SC_OK;

    /* Try payload.body.data first (simple messages) */
    sc_json_value_t *body_obj = sc_json_object_get(payload, "body");
    if (body_obj && body_obj->type == SC_JSON_OBJECT) {
        const char *data = sc_json_get_string(body_obj, "data");
        if (data && strlen(data) > 0) {
            size_t dlen = strlen(data);
            char *decoded = (char *)alloc->alloc(alloc->ctx, dlen + 1);
            if (!decoded)
                return SC_ERR_OUT_OF_MEMORY;
            size_t dec_len = 0;
            sc_error_t err = base64url_decode(data, dlen, decoded, dlen + 1, &dec_len);
            if (err != SC_OK) {
                alloc->free(alloc->ctx, decoded, dlen + 1);
                return err;
            }
            size_t copy = dec_len < out_cap - 1 ? dec_len : out_cap - 1;
            memcpy(out, decoded, copy);
            out[copy] = '\0';
            *out_len = copy;
            alloc->free(alloc->ctx, decoded, dlen + 1);
            return SC_OK;
        }
    }

    /* Try payload.parts (multipart) — prefer text/plain */
    sc_json_value_t *parts = sc_json_object_get(payload, "parts");
    if (parts && parts->type == SC_JSON_ARRAY) {
        for (size_t i = 0; i < parts->data.array.len; i++) {
            sc_json_value_t *part = parts->data.array.items[i];
            if (!part || part->type != SC_JSON_OBJECT)
                continue;
            const char *mime = sc_json_get_string(part, "mimeType");
            if (!mime ||
                (strcasecmp(mime, "text/plain") != 0 && strcasecmp(mime, "text/html") != 0))
                continue;
            sc_json_value_t *pbody = sc_json_object_get(part, "body");
            if (!pbody || pbody->type != SC_JSON_OBJECT)
                continue;
            const char *data = sc_json_get_string(pbody, "data");
            if (!data || strlen(data) == 0)
                continue;
            size_t dlen = strlen(data);
            char *decoded = (char *)alloc->alloc(alloc->ctx, dlen + 1);
            if (!decoded)
                return SC_ERR_OUT_OF_MEMORY;
            size_t dec_len = 0;
            sc_error_t err = base64url_decode(data, dlen, decoded, dlen + 1, &dec_len);
            if (err != SC_OK) {
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
            return SC_OK;
        }
    }
    return SC_OK;
}
#endif

/* ─── Vtable ────────────────────────────────────────────────────────────── */

static sc_error_t gmail_start(void *ctx) {
    sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void gmail_stop(void *ctx) {
    sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t gmail_send(void *ctx, const char *target, size_t target_len, const char *message,
                             size_t message_len, const char *const *media, size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_ERR_NOT_SUPPORTED; /* read-only channel */
}

static const char *gmail_name(void *ctx) {
    (void)ctx;
    return "gmail";
}

static bool gmail_health_check(void *ctx) {
    sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)ctx;
    if (!c)
        return false;
    return c->access_token != NULL && c->access_token_len > 0;
}

static const sc_channel_vtable_t gmail_vtable = {
    .start = gmail_start,
    .stop = gmail_stop,
    .send = gmail_send,
    .name = gmail_name,
    .health_check = gmail_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

/* ─── Public API ─────────────────────────────────────────────────────────── */

sc_error_t sc_gmail_create(sc_allocator_t *alloc, const char *client_id, size_t client_id_len,
                           const char *client_secret, size_t client_secret_len,
                           const char *refresh_token, size_t refresh_token_len,
                           int poll_interval_sec, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->poll_interval_sec = poll_interval_sec > 0 ? poll_interval_sec : 60;

    if (client_id && client_id_len > 0) {
        c->client_id = sc_strndup(alloc, client_id, client_id_len);
        if (!c->client_id) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
    }
    if (client_secret && client_secret_len > 0) {
        c->client_secret = sc_strndup(alloc, client_secret, client_secret_len);
        if (!c->client_secret) {
            if (c->client_id)
                alloc->free(alloc->ctx, c->client_id, strlen(c->client_id) + 1);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
    }
    if (refresh_token && refresh_token_len > 0) {
        c->refresh_token = sc_strndup(alloc, refresh_token, refresh_token_len);
        if (!c->refresh_token) {
            if (c->client_id)
                alloc->free(alloc->ctx, c->client_id, strlen(c->client_id) + 1);
            if (c->client_secret)
                alloc->free(alloc->ctx, c->client_secret, strlen(c->client_secret) + 1);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
    }

    out->ctx = c;
    out->vtable = &gmail_vtable;
    return SC_OK;
}

void sc_gmail_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)ch->ctx;
        if (c->client_id)
            c->alloc->free(c->alloc->ctx, c->client_id, strlen(c->client_id) + 1);
        if (c->client_secret)
            c->alloc->free(c->alloc->ctx, c->client_secret, strlen(c->client_secret) + 1);
        if (c->refresh_token)
            c->alloc->free(c->alloc->ctx, c->refresh_token, strlen(c->refresh_token) + 1);
        if (c->access_token)
            c->alloc->free(c->alloc->ctx, c->access_token, c->access_token_len + 1);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

bool sc_gmail_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)ch->ctx;
    return c->client_id != NULL && c->client_secret != NULL && c->refresh_token != NULL;
}

/* ─── Poll ──────────────────────────────────────────────────────────────── */

sc_error_t sc_gmail_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                         size_t max_msgs, size_t *out_count) {
    sc_gmail_ctx_t *c = (sc_gmail_ctx_t *)channel_ctx;
    if (!c || !alloc || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;

#if SC_IS_TEST
    (void)max_msgs;
    return SC_OK;
#elif !defined(SC_HTTP_CURL)
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#else
    if (!c->client_id || !c->client_secret || !c->refresh_token)
        return SC_OK;

    sc_error_t err = ensure_access_token(c);
    if (err != SC_OK)
        return err;

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", c->access_token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return SC_ERR_INTERNAL;

    /* GET messages?q=is:unread&maxResults=10 */
    char list_url[384];
    int nu = snprintf(list_url, sizeof(list_url), "%s/messages?q=is:unread&maxResults=10",
                      GMAIL_API_BASE);
    if (nu <= 0 || (size_t)nu >= sizeof(list_url))
        return SC_ERR_INTERNAL;

    sc_http_response_t resp = {0};
    err = sc_http_get(alloc, list_url, auth_buf, &resp);
    if (err != SC_OK) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        return err;
    }
    if (!resp.body || resp.body_len == 0 || resp.status_code != 200) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        return SC_OK;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    if (resp.owned && resp.body)
        sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed)
        return SC_OK;

    sc_json_value_t *messages_arr = sc_json_object_get(parsed, "messages");
    if (!messages_arr || messages_arr->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, parsed);
        return SC_OK;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < messages_arr->data.array.len && cnt < max_msgs; i++) {
        sc_json_value_t *msg_ref = messages_arr->data.array.items[i];
        if (!msg_ref || msg_ref->type != SC_JSON_OBJECT)
            continue;
        const char *msg_id = sc_json_get_string(msg_ref, "id");
        if (!msg_id || !msg_id[0])
            continue;

        /* GET full message */
        char get_url[384];
        int ng = snprintf(get_url, sizeof(get_url), "%s/messages/%.100s?format=full",
                          GMAIL_API_BASE, msg_id);
        if (ng <= 0 || (size_t)ng >= sizeof(get_url))
            continue;

        sc_http_response_t gresp = {0};
        err = sc_http_get(alloc, get_url, auth_buf, &gresp);
        if (err != SC_OK || !gresp.body || gresp.body_len == 0 || gresp.status_code != 200) {
            if (gresp.owned && gresp.body)
                sc_http_response_free(alloc, &gresp);
            continue;
        }

        sc_json_value_t *msg_full = NULL;
        err = sc_json_parse(alloc, gresp.body, gresp.body_len, &msg_full);
        if (gresp.owned && gresp.body)
            sc_http_response_free(alloc, &gresp);
        if (err != SC_OK || !msg_full)
            continue;

        sc_json_value_t *payload = sc_json_object_get(msg_full, "payload");
        if (!payload || payload->type != SC_JSON_OBJECT) {
            sc_json_free(alloc, msg_full);
            continue;
        }

        sc_json_value_t *headers = sc_json_object_get(payload, "headers");
        const char *from = get_header_value(headers, "From");
        const char *subject = get_header_value(headers, "Subject");
        if (!from)
            from = "";
        if (!subject)
            subject = "";

        char body_buf[GMAIL_CONTENT_MAX + 1];
        size_t body_len = 0;
        err = extract_body_from_payload(alloc, payload, body_buf, sizeof(body_buf), &body_len);
        if (err != SC_OK) {
            sc_json_free(alloc, msg_full);
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

        sc_json_free(alloc, msg_full);

        /* Mark as read: POST modify with removeLabelIds: ["UNREAD"] */
        char mod_url[384];
        int nm =
            snprintf(mod_url, sizeof(mod_url), "%s/messages/%.100s/modify", GMAIL_API_BASE, msg_id);
        if (nm > 0 && (size_t)nm < sizeof(mod_url)) {
            const char *mod_body = "{\"removeLabelIds\":[\"UNREAD\"]}";
            sc_http_response_t mresp = {0};
            sc_error_t mod_err = sc_http_post_json(alloc, mod_url, auth_buf, mod_body, 29, &mresp);
            if (mresp.owned && mresp.body)
                sc_http_response_free(alloc, &mresp);
            if (mod_err != SC_OK) {
                sc_json_free(alloc, parsed);
                *out_count = cnt;
                return mod_err;
            }
        }
    }

    sc_json_free(alloc, parsed);
    *out_count = cnt;
    return SC_OK;
#endif
}
