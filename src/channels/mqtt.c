#include "seaclaw/channels/mqtt.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/string.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SC_MQTT_MAX_MSG      65536
#define SC_MQTT_LAST_MSG_LEN 4096
#define SC_MQTT_MOCK_MAX     8

typedef struct sc_mqtt_mock_msg {
    char session_key[128];
    char content[4096];
} sc_mqtt_mock_msg_t;

typedef struct sc_mqtt_ctx {
    sc_allocator_t *alloc;
    char *broker_url;
    char *inbound_topic;
    char *outbound_topic;
    char *username;
    char *password;
    int qos;
    bool running;
#if SC_IS_TEST
    char last_message[SC_MQTT_LAST_MSG_LEN];
    size_t last_message_len;
    size_t mock_count;
    sc_mqtt_mock_msg_t mock_msgs[SC_MQTT_MOCK_MAX];
#endif
} sc_mqtt_ctx_t;

static sc_error_t mqtt_start(void *ctx) {
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    c->running = true;
    return SC_OK;
#else
    if (!c->broker_url || c->broker_url[0] == '\0')
        return SC_ERR_NOT_SUPPORTED;
    c->running = true;
    return SC_OK;
#endif
}

static void mqtt_stop(void *ctx) {
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static sc_error_t mqtt_send(void *ctx, const char *target, size_t target_len, const char *message,
                            size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ctx;
    if (c && message && message_len > 0) {
        size_t copy = message_len;
        if (copy >= SC_MQTT_LAST_MSG_LEN)
            copy = SC_MQTT_LAST_MSG_LEN - 1;
        memcpy(c->last_message, message, copy);
        c->last_message[copy] = '\0';
        c->last_message_len = copy;
    }
    return SC_OK;
#else
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ctx;
    if (!c || !c->alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (!c->broker_url || c->broker_url[0] == '\0')
        return SC_ERR_NOT_SUPPORTED;
    if (!message || message_len > SC_MQTT_MAX_MSG)
        return SC_ERR_INVALID_ARGUMENT;
    (void)target;
    (void)target_len;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static const char *mqtt_name(void *ctx) {
    (void)ctx;
    return "mqtt";
}

static bool mqtt_health_check(void *ctx) {
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ctx;
#if SC_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static const sc_channel_vtable_t mqtt_vtable = {
    .start = mqtt_start,
    .stop = mqtt_stop,
    .send = mqtt_send,
    .name = mqtt_name,
    .health_check = mqtt_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_mqtt_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count) {
    sc_mqtt_ctx_t *ctx = (sc_mqtt_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    (void)alloc;
    for (size_t i = 0; i < ctx->mock_count && i < max_msgs; i++) {
        size_t sk = strlen(ctx->mock_msgs[i].session_key);
        if (sk > 127)
            sk = 127;
        memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, sk);
        msgs[i].session_key[sk] = '\0';
        size_t ct = strlen(ctx->mock_msgs[i].content);
        if (ct > 4095)
            ct = 4095;
        memcpy(msgs[i].content, ctx->mock_msgs[i].content, ct);
        msgs[i].content[ct] = '\0';
    }
    *out_count = ctx->mock_count > max_msgs ? max_msgs : ctx->mock_count;
    return SC_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

#if SC_IS_TEST
sc_error_t sc_mqtt_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                    size_t session_key_len, const char *content,
                                    size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ch->ctx;
    if (c->mock_count >= SC_MQTT_MOCK_MAX)
        return SC_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return SC_OK;
}
#endif

sc_error_t sc_mqtt_create(sc_allocator_t *alloc, const char *broker_url, size_t broker_url_len,
                          const char *inbound_topic, size_t inbound_topic_len,
                          const char *outbound_topic, size_t outbound_topic_len,
                          const char *username, size_t username_len, const char *password,
                          size_t password_len, int qos, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (!broker_url || broker_url_len == 0)
        return SC_ERR_INVALID_ARGUMENT;

    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->qos = qos;

    c->broker_url = sc_strndup(alloc, broker_url, broker_url_len);
    if (!c->broker_url)
        goto oom;
    if (inbound_topic && inbound_topic_len > 0) {
        c->inbound_topic = sc_strndup(alloc, inbound_topic, inbound_topic_len);
        if (!c->inbound_topic)
            goto oom;
    }
    if (outbound_topic && outbound_topic_len > 0) {
        c->outbound_topic = sc_strndup(alloc, outbound_topic, outbound_topic_len);
        if (!c->outbound_topic)
            goto oom;
    }
    if (username && username_len > 0) {
        c->username = sc_strndup(alloc, username, username_len);
        if (!c->username)
            goto oom;
    }
    if (password && password_len > 0) {
        c->password = sc_strndup(alloc, password, password_len);
        if (!c->password)
            goto oom;
    }

    out->ctx = c;
    out->vtable = &mqtt_vtable;
    return SC_OK;
oom:
    if (c->broker_url)
        alloc->free(alloc->ctx, c->broker_url, broker_url_len + 1);
    if (c->inbound_topic)
        alloc->free(alloc->ctx, c->inbound_topic, inbound_topic_len + 1);
    if (c->outbound_topic)
        alloc->free(alloc->ctx, c->outbound_topic, outbound_topic_len + 1);
    if (c->username)
        alloc->free(alloc->ctx, c->username, username_len + 1);
    if (c->password)
        alloc->free(alloc->ctx, c->password, password_len + 1);
    free(c);
    return SC_ERR_OUT_OF_MEMORY;
}

void sc_mqtt_destroy(sc_channel_t *ch, sc_allocator_t *alloc) {
    if (!ch || !ch->ctx)
        return;
    sc_mqtt_ctx_t *c = (sc_mqtt_ctx_t *)ch->ctx;
    sc_allocator_t *a = alloc ? alloc : c->alloc;
    if (a) {
        if (c->broker_url) {
            size_t n = strlen(c->broker_url) + 1;
            a->free(a->ctx, c->broker_url, n);
        }
        if (c->inbound_topic) {
            size_t n = strlen(c->inbound_topic) + 1;
            a->free(a->ctx, c->inbound_topic, n);
        }
        if (c->outbound_topic) {
            size_t n = strlen(c->outbound_topic) + 1;
            a->free(a->ctx, c->outbound_topic, n);
        }
        if (c->username) {
            size_t n = strlen(c->username) + 1;
            a->free(a->ctx, c->username, n);
        }
        if (c->password) {
            size_t n = strlen(c->password) + 1;
            a->free(a->ctx, c->password, n);
        }
    }
    free(c);
    ch->ctx = NULL;
    ch->vtable = NULL;
}
