#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/channels/twilio.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/multimodal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TWILIO_API_BASE        "https://api.twilio.com/2010-04-01/Accounts/"
#define TWILIO_QUEUE_MAX       32
#define TWILIO_SESSION_KEY_MAX 127
#define TWILIO_CONTENT_MAX     4095
#define TWILIO_FORM_BUF_MAX    8192

typedef struct sc_twilio_queued_msg {
    char session_key[128];
    char content[4096];
} sc_twilio_queued_msg_t;

typedef struct sc_twilio_ctx {
    sc_allocator_t *alloc;
    char *account_sid;
    size_t account_sid_len;
    char *auth_token;
    size_t auth_token_len;
    char *from_number;
    size_t from_number_len;
    char *to_number;
    size_t to_number_len;
    bool running;
    sc_twilio_queued_msg_t queue[TWILIO_QUEUE_MAX];
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
} sc_twilio_ctx_t;

static sc_error_t twilio_start(void *ctx) {
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void twilio_stop(void *ctx) {
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)ctx;
    if (c)
        c->running = false;
}

#if !SC_IS_TEST
/* Percent-encode for application/x-www-form-urlencoded; append to buf, return new len or 0 on overflow. */
static size_t form_append_encoded(char *buf, size_t cap, size_t len, const char *key,
                                  size_t key_len, const char *val, size_t val_len) {
    static const char hex[] = "0123456789ABCDEF";
    if (len > 0)
        buf[len++] = '&';
    if (len + key_len + 1 >= cap)
        return 0;
    memcpy(buf + len, key, key_len);
    len += key_len;
    buf[len++] = '=';
    for (size_t i = 0; i < val_len && len + 3 < cap; i++) {
        unsigned char u = (unsigned char)val[i];
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
            u == '-' || u == '_' || u == '.' || u == '~') {
            buf[len++] = (char)u;
        } else {
            if (len + 3 > cap)
                return 0;
            buf[len++] = '%';
            buf[len++] = hex[u >> 4];
            buf[len++] = hex[u & 0x0F];
        }
    }
    return len;
}
#endif

static void twilio_queue_push(sc_twilio_ctx_t *c, const char *from, size_t from_len,
                              const char *body, size_t body_len) {
    if (c->queue_count >= TWILIO_QUEUE_MAX)
        return;
    sc_twilio_queued_msg_t *slot = &c->queue[c->queue_tail];
    size_t sk = from_len < TWILIO_SESSION_KEY_MAX ? from_len : TWILIO_SESSION_KEY_MAX;
    memcpy(slot->session_key, from, sk);
    slot->session_key[sk] = '\0';
    size_t ct = body_len < TWILIO_CONTENT_MAX ? body_len : TWILIO_CONTENT_MAX;
    memcpy(slot->content, body, ct);
    slot->content[ct] = '\0';
    c->queue_tail = (c->queue_tail + 1) % TWILIO_QUEUE_MAX;
    c->queue_count++;
}

static sc_error_t twilio_send(void *ctx, const char *target, size_t target_len, const char *message,
                              size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)ctx;

#if SC_IS_TEST
    (void)target;
    (void)target_len;
    if (!c || !message)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->account_sid || c->account_sid_len == 0 || !c->auth_token || c->auth_token_len == 0 ||
        !c->from_number || c->from_number_len == 0 || !c->to_number || c->to_number_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    twilio_queue_push(c, "test-sender", 11, message, message_len);
    return SC_OK;
#else
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->account_sid || c->account_sid_len == 0 || !c->auth_token || c->auth_token_len == 0 ||
        !c->from_number || c->from_number_len == 0 || !c->to_number || c->to_number_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!message)
        return SC_ERR_INVALID_ARGUMENT;

    const char *to = (target && target_len > 0) ? target : c->to_number;
    size_t to_len = (target && target_len > 0) ? target_len : c->to_number_len;
    if (!to || to_len == 0)
        return SC_ERR_INVALID_ARGUMENT;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%.*s/Messages.json", TWILIO_API_BASE,
                     (int)c->account_sid_len, c->account_sid);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    char form_buf[TWILIO_FORM_BUF_MAX];
    size_t flen = 0;
    flen = form_append_encoded(form_buf, sizeof(form_buf), flen, "To", 2, to, to_len);
    if (flen == 0)
        return SC_ERR_INTERNAL;
    flen = form_append_encoded(form_buf, sizeof(form_buf), flen, "From", 4, c->from_number,
                               c->from_number_len);
    if (flen == 0)
        return SC_ERR_INTERNAL;
    flen = form_append_encoded(form_buf, sizeof(form_buf), flen, "Body", 4, message, message_len);
    if (flen == 0)
        return SC_ERR_INTERNAL;

    size_t cred_len = c->account_sid_len + 1 + c->auth_token_len;
    char *cred = (char *)c->alloc->alloc(c->alloc->ctx, cred_len + 1);
    if (!cred)
        return SC_ERR_OUT_OF_MEMORY;
    memcpy(cred, c->account_sid, c->account_sid_len);
    cred[c->account_sid_len] = ':';
    memcpy(cred + c->account_sid_len + 1, c->auth_token, c->auth_token_len + 1);

    char *b64 = NULL;
    size_t b64_len = 0;
    sc_error_t err = sc_multimodal_encode_base64(c->alloc, cred, cred_len, &b64, &b64_len);
    c->alloc->free(c->alloc->ctx, cred, cred_len + 1);
    if (err != SC_OK)
        return SC_ERR_INTERNAL;

    char auth_buf[768];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Authorization: Basic %.*s",
                      (int)b64_len, b64);
    c->alloc->free(c->alloc->ctx, b64, b64_len + 1);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf))
        return SC_ERR_INTERNAL;

    char headers_buf[1024];
    n = snprintf(headers_buf, sizeof(headers_buf),
                 "Content-Type: application/x-www-form-urlencoded\r\n%s", auth_buf);
    if (n <= 0 || (size_t)n >= sizeof(headers_buf))
        return SC_ERR_INTERNAL;

    sc_http_response_t resp = {0};
    err = sc_http_request(c->alloc, url_buf, "POST", headers_buf, form_buf, flen, &resp);
    if (err != SC_OK) {
        if (resp.owned && resp.body)
            sc_http_response_free(c->alloc, &resp);
        return SC_ERR_CHANNEL_SEND;
    }
    if (resp.owned && resp.body)
        sc_http_response_free(c->alloc, &resp);
    if (resp.status_code < 200 || resp.status_code >= 300)
        return SC_ERR_CHANNEL_SEND;
    return SC_OK;
#endif
}

static const char *twilio_name(void *ctx) {
    (void)ctx;
    return "twilio";
}

static bool twilio_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t twilio_vtable = {
    .start = twilio_start,
    .stop = twilio_stop,
    .send = twilio_send,
    .name = twilio_name,
    .health_check = twilio_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_twilio_on_webhook(void *channel_ctx, sc_allocator_t *alloc, const char *body,
                                size_t body_len) {
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)channel_ctx;
    if (!c || !body || body_len == 0)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)alloc;
    twilio_queue_push(c, "test-sender", 11, body, body_len);
    return SC_OK;
#else
    (void)alloc;
    const char *from_start = strstr(body, "From=");
    const char *body_start = strstr(body, "Body=");
    if (!from_start || !body_start)
        return SC_OK;
    from_start += 5;
    body_start += 5;
    const char *from_end = strchr(from_start, '&');
    const char *body_end = strchr(body_start, '&');
    size_t from_len = from_end ? (size_t)(from_end - from_start) : strlen(from_start);
    size_t msg_len = body_end ? (size_t)(body_end - body_start) : strlen(body_start);
    if (from_len > 0 && msg_len > 0)
        twilio_queue_push(c, from_start, from_len, body_start, msg_len);
    return SC_OK;
#endif
}

sc_error_t sc_twilio_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                          size_t max_msgs, size_t *out_count) {
    (void)alloc;
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)channel_ctx;
    if (!c || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    size_t cnt = 0;
    while (c->queue_count > 0 && cnt < max_msgs) {
        sc_twilio_queued_msg_t *slot = &c->queue[c->queue_head];
        memcpy(msgs[cnt].session_key, slot->session_key, sizeof(slot->session_key));
        memcpy(msgs[cnt].content, slot->content, sizeof(slot->content));
        c->queue_head = (c->queue_head + 1) % TWILIO_QUEUE_MAX;
        c->queue_count--;
        cnt++;
    }
    *out_count = cnt;
    return SC_OK;
}

bool sc_twilio_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx)
        return false;
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)ch->ctx;
    return c->account_sid != NULL && c->account_sid_len > 0 &&
           c->auth_token != NULL && c->auth_token_len > 0 &&
           c->from_number != NULL && c->from_number_len > 0 &&
           c->to_number != NULL && c->to_number_len > 0;
}

sc_error_t sc_twilio_create(sc_allocator_t *alloc, const char *account_sid, size_t account_sid_len,
                            const char *auth_token, size_t auth_token_len, const char *from_number,
                            size_t from_number_len, const char *to_number, size_t to_number_len,
                            sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (account_sid && account_sid_len > 0) {
        c->account_sid = (char *)malloc(account_sid_len + 1);
        if (!c->account_sid) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->account_sid, account_sid, account_sid_len);
        c->account_sid[account_sid_len] = '\0';
        c->account_sid_len = account_sid_len;
    }
    if (auth_token && auth_token_len > 0) {
        c->auth_token = (char *)malloc(auth_token_len + 1);
        if (!c->auth_token) {
            if (c->account_sid)
                free(c->account_sid);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->auth_token, auth_token, auth_token_len);
        c->auth_token[auth_token_len] = '\0';
        c->auth_token_len = auth_token_len;
    }
    if (from_number && from_number_len > 0) {
        c->from_number = (char *)malloc(from_number_len + 1);
        if (!c->from_number) {
            if (c->auth_token)
                free(c->auth_token);
            if (c->account_sid)
                free(c->account_sid);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->from_number, from_number, from_number_len);
        c->from_number[from_number_len] = '\0';
        c->from_number_len = from_number_len;
    }
    if (to_number && to_number_len > 0) {
        c->to_number = (char *)malloc(to_number_len + 1);
        if (!c->to_number) {
            if (c->from_number)
                free(c->from_number);
            if (c->auth_token)
                free(c->auth_token);
            if (c->account_sid)
                free(c->account_sid);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->to_number, to_number, to_number_len);
        c->to_number[to_number_len] = '\0';
        c->to_number_len = to_number_len;
    }
    out->ctx = c;
    out->vtable = &twilio_vtable;
    return SC_OK;
}

void sc_twilio_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_twilio_ctx_t *c = (sc_twilio_ctx_t *)ch->ctx;
        if (c->account_sid)
            free(c->account_sid);
        if (c->auth_token)
            free(c->auth_token);
        if (c->from_number)
            free(c->from_number);
        if (c->to_number)
            free(c->to_number);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
