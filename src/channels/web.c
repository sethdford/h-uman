/*
 * Web channel — token validation, streaming events (assistant_chunk/assistant_final).
 * HU_IS_TEST: all transport no-op. Production: stores events for connected clients.
 */
#include "human/channels/web.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <string.h>

#define HU_WEB_MAX_TOKEN_LEN      128
#define HU_WEB_ACCOUNT_ID_DEFAULT "default"

typedef struct hu_web_ctx {
    hu_allocator_t *alloc;
    char *last_response;
    size_t last_response_len;
    char *last_event;
    size_t last_event_len;
    bool running;
    char auth_token[HU_WEB_MAX_TOKEN_LEN];
    size_t auth_token_len;
    bool token_initialized;
    size_t connection_count;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_web_ctx_t;

static hu_error_t web_start(void *ctx) {
    hu_web_ctx_t *c = (hu_web_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void web_stop(void *ctx) {
    hu_web_ctx_t *c = (hu_web_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static void free_last_event(hu_web_ctx_t *c) {
    if (c->last_event && c->alloc) {
        c->alloc->free(c->alloc->ctx, c->last_event, c->last_event_len + 1);
        c->last_event = NULL;
        c->last_event_len = 0;
    }
}

#if !HU_IS_TEST
/* Build streaming event JSON: {"v":1,"type":"assistant_chunk"|"assistant_final",...} */
static hu_error_t build_event_json(hu_allocator_t *alloc, const char *target, size_t target_len,
                                   const char *message, size_t message_len,
                                   hu_outbound_stage_t stage, char **out, size_t *out_len) {
    const char *event_type =
        (stage == HU_OUTBOUND_STAGE_CHUNK) ? "assistant_chunk" : "assistant_final";

    hu_json_buf_t jbuf;
    hu_error_t err = hu_json_buf_init(&jbuf, alloc);
    if (err)
        return err;

    err = hu_json_buf_append_raw(&jbuf, "{\"v\":1,\"type\":", 14);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, event_type, strlen(event_type));
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"session_id\":", 14);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, target, target_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"agent_id\":", 12);
    if (err)
        goto fail;
    err =
        hu_json_append_string(&jbuf, HU_WEB_ACCOUNT_ID_DEFAULT, strlen(HU_WEB_ACCOUNT_ID_DEFAULT));
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, ",\"payload\":{\"content\":", 22);
    if (err)
        goto fail;
    err = hu_json_append_string(&jbuf, message, message_len);
    if (err)
        goto fail;
    err = hu_json_buf_append_raw(&jbuf, "}}", 2);
    if (err)
        goto fail;

    *out_len = jbuf.len;
    *out = (char *)alloc->alloc(alloc->ctx, jbuf.len + 1);
    if (!*out) {
        err = HU_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    memcpy(*out, jbuf.ptr, jbuf.len + 1);
    hu_json_buf_free(&jbuf);
    return HU_OK;
fail:
    hu_json_buf_free(&jbuf);
    return err;
}
#endif

static hu_error_t web_send(void *ctx, const char *target, size_t target_len, const char *message,
                           size_t message_len, const char *const *media, size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
#if HU_IS_TEST
    {
        hu_web_ctx_t *c = (hu_web_ctx_t *)ctx;
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return HU_OK;
    }
#else
    hu_web_ctx_t *c = (hu_web_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    free_last_event(c);
    if (c->last_response) {
        c->alloc->free(c->alloc->ctx, c->last_response, c->last_response_len + 1);
        c->last_response = NULL;
        c->last_response_len = 0;
    }
    c->last_response = hu_strndup(c->alloc, message, message_len);
    if (!c->last_response && message_len > 0)
        return HU_ERR_OUT_OF_MEMORY;
    c->last_response_len = c->last_response ? strlen(c->last_response) : 0;
    return HU_OK;
#endif
}

static hu_error_t web_send_event(void *ctx, const char *target, size_t target_len,
                                 const char *message, size_t message_len, const char *const *media,
                                 size_t media_count, hu_outbound_stage_t stage) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
#if HU_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    (void)stage;
    return HU_OK;
#else
    hu_web_ctx_t *c = (hu_web_ctx_t *)ctx;
    if (!c || !c->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    char *event_json = NULL;
    size_t event_len = 0;
    hu_error_t err = build_event_json(c->alloc, target, target_len, message, message_len, stage,
                                      &event_json, &event_len);
    if (err || !event_json)
        return err;

    free_last_event(c);
    c->last_event = event_json;
    c->last_event_len = event_len;

    if (c->connection_count > 0) {
        /* Would broadcast to WebSocket connections here. */
        (void)c->connection_count;
    }
    return HU_OK;
#endif
}

static const char *web_name(void *ctx) {
    (void)ctx;
    return "web";
}

static bool web_health_check(void *ctx) {
    hu_web_ctx_t *c = (hu_web_ctx_t *)ctx;
#if HU_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static const hu_channel_vtable_t web_vtable = {
    .start = web_start,
    .stop = web_stop,
    .send = web_send,
    .name = web_name,
    .health_check = web_health_check,
    .send_event = web_send_event,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_web_create(hu_allocator_t *alloc, hu_channel_t *out) {
    return hu_web_create_with_token(alloc, NULL, 0, out);
}

hu_error_t hu_web_create_with_token(hu_allocator_t *alloc, const char *auth_token,
                                    size_t auth_token_len, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_web_ctx_t *c = (hu_web_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;

    if (auth_token && auth_token_len > 0 && auth_token_len < HU_WEB_MAX_TOKEN_LEN) {
        size_t len = auth_token_len;
        while (len > 0 && (auth_token[len - 1] == ' ' || auth_token[len - 1] == '\t' ||
                           auth_token[len - 1] == '\r' || auth_token[len - 1] == '\n'))
            len--;
        size_t start = 0;
        while (start < len && (auth_token[start] == ' ' || auth_token[start] == '\t' ||
                               auth_token[start] == '\r' || auth_token[start] == '\n'))
            start++;
        if (len > start) {
            memcpy(c->auth_token, auth_token + start, len - start);
            c->auth_token_len = len - start;
            c->auth_token[c->auth_token_len] = '\0';
            c->token_initialized = true;
        }
    }

    out->ctx = c;
    out->vtable = &web_vtable;
    return HU_OK;
}

bool hu_web_validate_token(const hu_channel_t *ch, const char *candidate, size_t candidate_len) {
    if (!ch || !ch->ctx)
        return false;
    hu_web_ctx_t *c = (hu_web_ctx_t *)ch->ctx;
    if (!c->token_initialized)
        return false;
    if (candidate_len != c->auth_token_len)
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < candidate_len; i++)
        diff |= (unsigned char)candidate[i] ^ (unsigned char)c->auth_token[i];
    return diff == 0;
}

void hu_web_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_web_ctx_t *c = (hu_web_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        free_last_event(c);
        if (a && c->last_response) {
            a->free(a->ctx, c->last_response, c->last_response_len + 1);
        }
        if (a)
            a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_web_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                   size_t session_key_len, const char *content,
                                   size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_web_ctx_t *c = (hu_web_ctx_t *)ch->ctx;
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
const char *hu_web_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_web_ctx_t *c = (hu_web_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
