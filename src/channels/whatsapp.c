#include "seaclaw/channel.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHATSAPP_API_BASE "https://graph.facebook.com/v18.0/"

typedef struct sc_whatsapp_ctx {
    sc_allocator_t *alloc;
    char *phone_number_id;
    size_t phone_number_id_len;
    char *token;
    size_t token_len;
    bool running;
} sc_whatsapp_ctx_t;

static sc_error_t whatsapp_start(void *ctx) {
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void whatsapp_stop(void *ctx) {
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t whatsapp_send(void *ctx, const char *target, size_t target_len,
                                const char *message, size_t message_len, const char *const *media,
                                size_t media_count) {
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ctx;

#if SC_IS_TEST
    (void)c;
    return SC_OK;
#else
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->phone_number_id || c->phone_number_id_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!c->token || c->token_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;
    if (!target || target_len == 0 || !message)
        return SC_ERR_INVALID_ARGUMENT;

    char url_buf[512];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%.*s/messages", WHATSAPP_API_BASE,
                     (int)c->phone_number_id_len, c->phone_number_id);
    if (n < 0 || (size_t)n >= sizeof(url_buf))
        return SC_ERR_INTERNAL;

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, c->alloc);
    if (err)
        return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"messaging_product\":\"whatsapp\",", 32);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "to", 2, target, target_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, ",\"type\":\"text\",\"text\":{", 24);
    if (err)
        goto jfail;
    err = sc_json_append_key_value(&jbuf, "body", 4, message, message_len);
    if (err)
        goto jfail;
    err = sc_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto jfail;

    char auth_buf[512];
    n = snprintf(auth_buf, sizeof(auth_buf), "Bearer %.*s", (int)c->token_len, c->token);
    if (n <= 0 || (size_t)n >= sizeof(auth_buf)) {
        sc_json_buf_free(&jbuf);
        return SC_ERR_INTERNAL;
    }

    sc_http_response_t resp = {0};
    err = sc_http_post_json(c->alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
    sc_json_buf_free(&jbuf);
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
jfail:
    sc_json_buf_free(&jbuf);
    return err;
#endif
}

static const char *whatsapp_name(void *ctx) {
    (void)ctx;
    return "whatsapp";
}
static bool whatsapp_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t whatsapp_vtable = {
    .start = whatsapp_start,
    .stop = whatsapp_stop,
    .send = whatsapp_send,
    .name = whatsapp_name,
    .health_check = whatsapp_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_whatsapp_create(sc_allocator_t *alloc, const char *phone_number_id,
                              size_t phone_number_id_len, const char *token, size_t token_len,
                              sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (phone_number_id && phone_number_id_len > 0) {
        c->phone_number_id = (char *)malloc(phone_number_id_len + 1);
        if (!c->phone_number_id) {
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->phone_number_id, phone_number_id, phone_number_id_len);
        c->phone_number_id[phone_number_id_len] = '\0';
        c->phone_number_id_len = phone_number_id_len;
    }
    if (token && token_len > 0) {
        c->token = (char *)malloc(token_len + 1);
        if (!c->token) {
            if (c->phone_number_id)
                free(c->phone_number_id);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->token, token, token_len);
        c->token[token_len] = '\0';
        c->token_len = token_len;
    }
    out->ctx = c;
    out->vtable = &whatsapp_vtable;
    return SC_OK;
}

void sc_whatsapp_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_whatsapp_ctx_t *c = (sc_whatsapp_ctx_t *)ch->ctx;
        if (c->phone_number_id)
            free(c->phone_number_id);
        if (c->token)
            free(c->token);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
