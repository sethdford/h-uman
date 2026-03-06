/*
 * Instagram Posts tool — publish photos, list media, comment.
 */
#include "seaclaw/tools/instagram.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_INSTAGRAM_NAME "instagram_posts"
#define SC_INSTAGRAM_DESC "Publish photos, list media, and comment on Instagram posts"
#define SC_INSTAGRAM_PARAMS                                                                        \
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":["           \
    "\"publish_photo\",\"list_media\",\"comment\"]},\"account_id\":{\"type\":\"string\"},"         \
    "\"image_url\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"},\"media_id\":{"         \
    "\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"access_token\":{\"type\":\"string\"" \
    "}},\"required\":[\"operation\",\"account_id\"]}"

#define SC_IG_GRAPH_BASE "https://graph.facebook.com/v21.0/"

typedef struct sc_instagram_ctx {
    char _unused;
} sc_instagram_ctx_t;

static sc_error_t instagram_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                    sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out)
        return SC_ERR_INVALID_ARGUMENT;
    const char *op = sc_json_get_string(args, "operation");
    const char *account_id = sc_json_get_string(args, "account_id");
    if (!op || strlen(op) == 0 || !account_id || strlen(account_id) == 0) {
        *out = sc_tool_result_fail("Missing 'operation' or 'account_id'", 29);
        return SC_OK;
    }

#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "{\"success\":true,\"id\":\"mock_media_123\"}", 33);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 33);
    return SC_OK;
#else
    const char *token = sc_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        *out = sc_tool_result_fail("Missing 'access_token' for Instagram API", 40);
        return SC_OK;
    }

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf)) {
        *out = sc_tool_result_fail("auth header too long", 20);
        return SC_OK;
    }

    char url_buf[512];
    sc_json_buf_t jbuf;
    sc_http_response_t resp = {0};
    sc_error_t err;

    if (strcmp(op, "publish_photo") == 0) {
        const char *image_url = sc_json_get_string(args, "image_url");
        if (!image_url || strlen(image_url) == 0) {
            *out = sc_tool_result_fail("Missing 'image_url' for publish_photo", 37);
            return SC_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/media", SC_IG_GRAPH_BASE, account_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        err = sc_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = sc_tool_result_fail("out of memory", 12);
            return err;
        }
        err = sc_json_append_key_value(&jbuf, "image_url", 9, image_url, strlen(image_url));
        if (err)
            goto jfail;
        {
            const char *caption = sc_json_get_string(args, "caption");
            if (caption && strlen(caption) > 0)
                err = sc_json_append_key_value(&jbuf, "caption", 7, caption, strlen(caption));
        }
        if (err)
            goto jfail;
        err = sc_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        sc_json_buf_free(&jbuf);
        if (err != SC_OK)
            goto resp_fail;
        if (resp.status_code < 200 || resp.status_code >= 300)
            goto resp_err;
        sc_json_value_t *parsed = NULL;
        err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        if (err != SC_OK || !parsed) {
            *out = sc_tool_result_fail("Invalid container response", 26);
            return SC_OK;
        }
        const char *creation_id = sc_json_get_string(parsed, "id");
        if (!creation_id) {
            sc_json_free(alloc, parsed);
            *out = sc_tool_result_fail("No container id in response", 28);
            return SC_OK;
        }
        n = snprintf(url_buf, sizeof(url_buf), "%s%s/media_publish", SC_IG_GRAPH_BASE, account_id);
        sc_json_free(alloc, parsed);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        err = sc_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = sc_tool_result_fail("out of memory", 12);
            return err;
        }
        err = sc_json_append_key_value(&jbuf, "creation_id", 10, creation_id, strlen(creation_id));
        if (err) {
            sc_json_buf_free(&jbuf);
            *out = sc_tool_result_fail("JSON build failed", 18);
            return err;
        }
        err = sc_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        sc_json_buf_free(&jbuf);
        if (err != SC_OK)
            goto resp_fail;
        if (resp.status_code < 200 || resp.status_code >= 300)
            goto resp_err;
    } else if (strcmp(op, "list_media") == 0) {
        int n =
            snprintf(url_buf, sizeof(url_buf), "%s%s/media?fields=id,caption,media_type,media_url",
                     SC_IG_GRAPH_BASE, account_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        err = sc_http_get(alloc, url_buf, auth_buf, &resp);
    } else if (strcmp(op, "comment") == 0) {
        const char *media_id = sc_json_get_string(args, "media_id");
        const char *message = sc_json_get_string(args, "message");
        if (!media_id || strlen(media_id) == 0 || !message || strlen(message) == 0) {
            *out = sc_tool_result_fail("Missing 'media_id' or 'message' for comment", 41);
            return SC_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/comments", SC_IG_GRAPH_BASE, media_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        err = sc_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = sc_tool_result_fail("out of memory", 12);
            return err;
        }
        err = sc_json_append_key_value(&jbuf, "message", 7, message, strlen(message));
        if (err) {
            sc_json_buf_free(&jbuf);
            *out = sc_tool_result_fail("JSON build failed", 18);
            return err;
        }
        err = sc_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        sc_json_buf_free(&jbuf);
    } else {
        *out = sc_tool_result_fail("Unknown operation", 17);
        return SC_OK;
    }

    if (err != SC_OK) {
    resp_fail:
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Instagram API request failed", 29);
        return SC_OK;
    }
resp_err:
    if (resp.status_code < 200 || resp.status_code >= 300) {
        char *rbody = resp.body && resp.body_len > 0 ? sc_strndup(alloc, resp.body, resp.body_len)
                                                     : sc_strndup(alloc, "API error", 9);
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        if (rbody)
            *out = sc_tool_result_fail_owned(rbody, strlen(rbody));
        else
            *out = sc_tool_result_fail("API error", 9);
        return SC_OK;
    }
    char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
    if (resp.owned && resp.body)
        sc_http_response_free(alloc, &resp);
    *out = sc_tool_result_ok_owned(rbody ? rbody : "{}", rbody ? strlen(rbody) : 2);
    return SC_OK;

jfail:
    sc_json_buf_free(&jbuf);
    *out = sc_tool_result_fail("JSON build failed", 18);
    return err;
#endif
}

static const char *instagram_name(void *ctx) {
    (void)ctx;
    return SC_INSTAGRAM_NAME;
}
static const char *instagram_description(void *ctx) {
    (void)ctx;
    return SC_INSTAGRAM_DESC;
}
static const char *instagram_parameters_json(void *ctx) {
    (void)ctx;
    return SC_INSTAGRAM_PARAMS;
}
static void instagram_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    if (ctx)
        free(ctx);
}

static const sc_tool_vtable_t instagram_vtable = {
    .execute = instagram_execute,
    .name = instagram_name,
    .description = instagram_description,
    .parameters_json = instagram_parameters_json,
    .deinit = instagram_deinit,
};

sc_error_t sc_instagram_tool_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_instagram_ctx_t *c = (sc_instagram_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = c;
    out->vtable = &instagram_vtable;
    return SC_OK;
}
