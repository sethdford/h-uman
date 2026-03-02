/*
 * Web channel — token validation, streaming events (assistant_chunk/assistant_final).
 * SC_IS_TEST: all transport no-op. Production: stores events for connected clients.
 */
#include "seaclaw/channels/web.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SC_WEB_MAX_TOKEN_LEN 128
#define SC_WEB_ACCOUNT_ID_DEFAULT "default"

typedef struct sc_web_ctx {
    sc_allocator_t *alloc;
    char *last_response;
    size_t last_response_len;
    char *last_event;
    size_t last_event_len;
    bool running;
    char auth_token[SC_WEB_MAX_TOKEN_LEN];
    size_t auth_token_len;
    bool token_initialized;
    size_t connection_count;
} sc_web_ctx_t;

static sc_error_t web_start(void *ctx)
{
    sc_web_ctx_t *c = (sc_web_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void web_stop(void *ctx)
{
    sc_web_ctx_t *c = (sc_web_ctx_t *)ctx;
    if (c) c->running = false;
}

static void free_last_event(sc_web_ctx_t *c)
{
    if (c->last_event && c->alloc) {
        c->alloc->free(c->alloc->ctx, c->last_event, c->last_event_len + 1);
        c->last_event = NULL;
        c->last_event_len = 0;
    }
}

#if !SC_IS_TEST
/* Build streaming event JSON: {"v":1,"type":"assistant_chunk"|"assistant_final",...} */
static sc_error_t build_event_json(sc_allocator_t *alloc,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    sc_outbound_stage_t stage,
    char **out, size_t *out_len)
{
    const char *event_type = (stage == SC_OUTBOUND_STAGE_CHUNK)
        ? "assistant_chunk" : "assistant_final";

    sc_json_buf_t jbuf;
    sc_error_t err = sc_json_buf_init(&jbuf, alloc);
    if (err) return err;

    err = sc_json_buf_append_raw(&jbuf, "{\"v\":1,\"type\":", 15);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, event_type, strlen(event_type));
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"session_id\":", 15);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, target, target_len);
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"agent_id\":", 12);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, SC_WEB_ACCOUNT_ID_DEFAULT, strlen(SC_WEB_ACCOUNT_ID_DEFAULT));
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, ",\"payload\":{\"content\":", 21);
    if (err) goto fail;
    err = sc_json_append_string(&jbuf, message, message_len);
    if (err) goto fail;
    err = sc_json_buf_append_raw(&jbuf, "}}", 2);
    if (err) goto fail;

    *out_len = jbuf.len;
    *out = (char *)alloc->alloc(alloc->ctx, jbuf.len + 1);
    if (!*out) { err = SC_ERR_OUT_OF_MEMORY; goto fail; }
    memcpy(*out, jbuf.ptr, jbuf.len + 1);
    sc_json_buf_free(&jbuf);
    return SC_OK;
fail:
    sc_json_buf_free(&jbuf);
    return err;
}
#endif

static sc_error_t web_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    return SC_OK;
#else
    sc_web_ctx_t *c = (sc_web_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;

    free_last_event(c);
    if (c->last_response) {
        c->alloc->free(c->alloc->ctx, c->last_response, c->last_response_len + 1);
        c->last_response = NULL;
        c->last_response_len = 0;
    }
    c->last_response = sc_strndup(c->alloc, message, message_len);
    if (!c->last_response && message_len > 0) return SC_ERR_OUT_OF_MEMORY;
    c->last_response_len = c->last_response ? strlen(c->last_response) : 0;
    return SC_OK;
#endif
}

static sc_error_t web_send_event(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count,
    sc_outbound_stage_t stage)
{
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    (void)media;
    (void)media_count;
    (void)stage;
    return SC_OK;
#else
    sc_web_ctx_t *c = (sc_web_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;

    char *event_json = NULL;
    size_t event_len = 0;
    sc_error_t err = build_event_json(c->alloc, target, target_len,
        message, message_len, stage, &event_json, &event_len);
    if (err || !event_json) return err;

    free_last_event(c);
    c->last_event = event_json;
    c->last_event_len = event_len;

    if (c->connection_count > 0) {
        /* Would broadcast to WebSocket connections here. */
        (void)c->connection_count;
    }
    return SC_OK;
#endif
}

static const char *web_name(void *ctx) { (void)ctx; return "web"; }

static bool web_health_check(void *ctx)
{
    sc_web_ctx_t *c = (sc_web_ctx_t *)ctx;
#if SC_IS_TEST
    return c != NULL;
#else
    return c && c->running;
#endif
}

static const sc_channel_vtable_t web_vtable = {
    .start = web_start,
    .stop = web_stop,
    .send = web_send,
    .name = web_name,
    .health_check = web_health_check,
    .send_event = web_send_event,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_web_create(sc_allocator_t *alloc, sc_channel_t *out)
{
    return sc_web_create_with_token(alloc, NULL, 0, out);
}

sc_error_t sc_web_create_with_token(sc_allocator_t *alloc,
    const char *auth_token, size_t auth_token_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_web_ctx_t *c = (sc_web_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;

    if (auth_token && auth_token_len > 0 && auth_token_len < SC_WEB_MAX_TOKEN_LEN) {
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
    return SC_OK;
}

bool sc_web_validate_token(const sc_channel_t *ch, const char *candidate, size_t candidate_len)
{
    if (!ch || !ch->ctx) return false;
    sc_web_ctx_t *c = (sc_web_ctx_t *)ch->ctx;
    if (!c->token_initialized) return false;
    if (candidate_len != c->auth_token_len) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < candidate_len; i++)
        diff |= (unsigned char)candidate[i] ^ (unsigned char)c->auth_token[i];
    return diff == 0;
}

void sc_web_destroy(sc_channel_t *ch)
{
    if (ch && ch->ctx) {
        sc_web_ctx_t *c = (sc_web_ctx_t *)ch->ctx;
        free_last_event(c);
        if (c->alloc && c->last_response) {
            c->alloc->free(c->alloc->ctx, c->last_response, c->last_response_len + 1);
        }
        free(ch->ctx);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
