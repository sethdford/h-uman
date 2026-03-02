#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_PUSHOVER_NAME "pushover"
#define SC_PUSHOVER_DESC "Send push notifications via Pushover."
#define SC_PUSHOVER_PARAMS "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"priority\":{\"type\":\"integer\"},\"sound\":{\"type\":\"string\"}},\"required\":[\"message\"]}"
#define SC_PUSHOVER_MSG_MAX 1024

typedef struct sc_pushover_ctx {
    char *api_token;
    size_t api_token_len;
    char *user_key;
    size_t user_key_len;
} sc_pushover_ctx_t;

static sc_error_t pushover_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_pushover_ctx_t *c = (sc_pushover_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *message = sc_json_get_string(args, "message");
    if (!message || strlen(message) == 0) {
        *out = sc_tool_result_fail("missing message", 14);
        return SC_OK;
    }
    if (strlen(message) > SC_PUSHOVER_MSG_MAX) {
        *out = sc_tool_result_fail("message too long", 17);
        return SC_OK;
    }
    const char *title = sc_json_get_string(args, "title");
    double priority_val = sc_json_get_number(args, "priority", 0);
    const char *sound = sc_json_get_string(args, "sound");
    (void)sound;
    if (priority_val < -2 || priority_val > 2) {
        *out = sc_tool_result_fail("priority must be -2 to 2", 26);
        return SC_OK;
    }
#if SC_IS_TEST
    (void)c;
    size_t need = 22 + (title && title[0] ? strlen(title) + 2 : 0) + strlen(message);
    char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    int n;
    if (title && title[0])
        n = snprintf(msg, need + 1, "Notification sent: %s: %s", title, message);
    else
        n = snprintf(msg, need + 1, "Notification sent: %s", message);
    size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
    msg[len] = '\0';
    *out = sc_tool_result_ok_owned(msg, len);
    return SC_OK;
#else
    if (!c->api_token || !c->api_token[0] || !c->user_key || !c->user_key[0]) {
        *out = sc_tool_result_fail("PUSHOVER_TOKEN and PUSHOVER_USER_KEY not configured", 46);
        return SC_OK;
    }
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "{", 1) != SC_OK) goto pw_fail;
    if (sc_json_append_key_value(&buf, "token", 5, c->api_token, c->api_token_len) != SC_OK) goto pw_fail;
    if (sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto pw_fail;
    if (sc_json_append_key_value(&buf, "user", 4, c->user_key, c->user_key_len) != SC_OK) goto pw_fail;
    if (sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto pw_fail;
    if (sc_json_append_key_value(&buf, "message", 7, message, strlen(message)) != SC_OK) goto pw_fail;
    if (title && title[0]) {
        if (sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto pw_fail;
        if (sc_json_append_key_value(&buf, "title", 5, title, strlen(title)) != SC_OK) goto pw_fail;
    }
    if (priority_val != 0) {
        if (sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto pw_fail;
        if (sc_json_append_key_int(&buf, "priority", 8, (long long)priority_val) != SC_OK) goto pw_fail;
    }
    if (sound && sound[0]) {
        if (sc_json_buf_append_raw(&buf, ",", 1) != SC_OK) goto pw_fail;
        if (sc_json_append_key_value(&buf, "sound", 5, sound, strlen(sound)) != SC_OK) goto pw_fail;
    }
    if (sc_json_buf_append_raw(&buf, "}", 1) != SC_OK) goto pw_fail;

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json(alloc, "https://api.pushover.net/1/messages.json", NULL, buf.ptr, buf.len, &resp);
    sc_json_buf_free(&buf);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Pushover request failed", 23);
        return SC_OK;
    }
    if (resp.status_code >= 200 && resp.status_code < 300) {
        char *msg = sc_strndup(alloc, "Notification sent successfully", 30);
        sc_http_response_free(alloc, &resp);
        *out = msg ? sc_tool_result_ok_owned(msg, 30) : sc_tool_result_fail("out of memory", 12);
    } else {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Pushover API returned error", 27);
    }
    return SC_OK;
pw_fail:
    sc_json_buf_free(&buf);
    *out = sc_tool_result_fail("out of memory", 12);
    return SC_ERR_OUT_OF_MEMORY;
#endif
}

static const char *pushover_name(void *ctx) { (void)ctx; return SC_PUSHOVER_NAME; }
static const char *pushover_description(void *ctx) { (void)ctx; return SC_PUSHOVER_DESC; }
static const char *pushover_parameters_json(void *ctx) { (void)ctx; return SC_PUSHOVER_PARAMS; }
static void pushover_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_pushover_ctx_t *c = (sc_pushover_ctx_t *)ctx;
    if (c) {
        if (c->api_token) free(c->api_token);
        if (c->user_key) free(c->user_key);
        free(c);
    }
}

static const sc_tool_vtable_t pushover_vtable = {
    .execute = pushover_execute, .name = pushover_name,
    .description = pushover_description, .parameters_json = pushover_parameters_json,
    .deinit = pushover_deinit,
};

sc_error_t sc_pushover_create(sc_allocator_t *alloc,
    const char *api_token, size_t api_token_len,
    const char *user_key, size_t user_key_len,
    sc_tool_t *out)
{
    (void)alloc;
    sc_pushover_ctx_t *c = (sc_pushover_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (api_token && api_token_len > 0) {
        c->api_token = (char *)malloc(api_token_len + 1);
        if (!c->api_token) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->api_token, api_token, api_token_len);
        c->api_token[api_token_len] = '\0';
        c->api_token_len = api_token_len;
    }
    if (user_key && user_key_len > 0) {
        c->user_key = (char *)malloc(user_key_len + 1);
        if (!c->user_key) {
            if (c->api_token) free(c->api_token);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->user_key, user_key, user_key_len);
        c->user_key[user_key_len] = '\0';
        c->user_key_len = user_key_len;
    }
    out->ctx = c;
    out->vtable = &pushover_vtable;
    return SC_OK;
}
