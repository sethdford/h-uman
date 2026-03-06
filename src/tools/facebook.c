/*
 * Facebook Pages tool — post, comment, list posts.
 */
#include "seaclaw/tools/facebook.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_FACEBOOK_NAME "facebook_pages"
#define SC_FACEBOOK_DESC "Post to Facebook Pages, comment on posts, list page posts"
#define SC_FACEBOOK_PARAMS                                                                         \
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":["           \
    "\"post\",\"comment\",\"list_posts\"]},\"page_id\":{\"type\":\"string\"},\"message\":{"        \
    "\"type\":\"string\"},\"post_id\":{\"type\":\"string\"},\"access_token\":{\"type\":\"string\"" \
    "}},\"required\":[\"operation\",\"page_id\"]}"

#define SC_FB_GRAPH_BASE "https://graph.facebook.com/v21.0/"

typedef struct sc_facebook_ctx {
    char _unused;
} sc_facebook_ctx_t;

static sc_error_t facebook_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                   sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *op = sc_json_get_string(args, "operation");
    const char *page_id = sc_json_get_string(args, "page_id");
    if (!op || strlen(op) == 0 || !page_id || strlen(page_id) == 0) {
        *out = sc_tool_result_fail("Missing 'operation' or 'page_id'", 30);
        return SC_OK;
    }

#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "{\"success\":true,\"id\":\"mock_123\"}", 33);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 33);
    return SC_OK;
#else
    const char *token = sc_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        *out = sc_tool_result_fail("Missing 'access_token' for Facebook API", 38);
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

    if (strcmp(op, "post") == 0) {
        const char *message = sc_json_get_string(args, "message");
        if (!message || strlen(message) == 0) {
            *out = sc_tool_result_fail("Missing 'message' for post", 25);
            return SC_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/feed", SC_FB_GRAPH_BASE, page_id);
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
    } else if (strcmp(op, "comment") == 0) {
        const char *post_id = sc_json_get_string(args, "post_id");
        const char *message = sc_json_get_string(args, "message");
        if (!post_id || strlen(post_id) == 0 || !message || strlen(message) == 0) {
            *out = sc_tool_result_fail("Missing 'post_id' or 'message' for comment", 41);
            return SC_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/comments", SC_FB_GRAPH_BASE, post_id);
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
    } else if (strcmp(op, "list_posts") == 0) {
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/feed?fields=id,message,created_time",
                         SC_FB_GRAPH_BASE, page_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        err = sc_http_get(alloc, url_buf, auth_buf, &resp);
    } else {
        *out = sc_tool_result_fail("Unknown operation", 17);
        return SC_OK;
    }

    if (err != SC_OK) {
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Facebook API request failed", 28);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        char *rbody = resp.body && resp.body_len > 0 ? sc_strndup(alloc, resp.body, resp.body_len)
                                                     : sc_strndup(alloc, "API error", 9);
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail_owned(rbody ? rbody : "API error", rbody ? strlen(rbody) : 9);
        return SC_OK;
    }
    char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
    if (resp.owned && resp.body)
        sc_http_response_free(alloc, &resp);
    *out = sc_tool_result_ok_owned(rbody ? rbody : "{}", rbody ? strlen(rbody) : 2);
    return SC_OK;
#endif
}

static const char *facebook_name(void *ctx) {
    (void)ctx;
    return SC_FACEBOOK_NAME;
}
static const char *facebook_description(void *ctx) {
    (void)ctx;
    return SC_FACEBOOK_DESC;
}
static const char *facebook_parameters_json(void *ctx) {
    (void)ctx;
    return SC_FACEBOOK_PARAMS;
}
static void facebook_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const sc_tool_vtable_t facebook_vtable = {
    .execute = facebook_execute,
    .name = facebook_name,
    .description = facebook_description,
    .parameters_json = facebook_parameters_json,
    .deinit = facebook_deinit,
};

sc_error_t sc_facebook_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_facebook_ctx_t *c = (sc_facebook_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = c;
    out->vtable = &facebook_vtable;
    return SC_OK;
}
