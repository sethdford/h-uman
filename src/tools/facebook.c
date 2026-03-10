/*
 * Facebook Pages tool — post, comment, list posts.
 */
#include "human/tools/facebook.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_FACEBOOK_NAME "facebook_pages"
#define HU_FACEBOOK_DESC "Post to Facebook Pages, comment on posts, list page posts"
#define HU_FACEBOOK_PARAMS                                                                         \
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":["           \
    "\"post\",\"comment\",\"list_posts\"]},\"page_id\":{\"type\":\"string\"},\"message\":{"        \
    "\"type\":\"string\"},\"post_id\":{\"type\":\"string\"},\"access_token\":{\"type\":\"string\"" \
    "}},\"required\":[\"operation\",\"page_id\"]}"

#define HU_FB_GRAPH_BASE "https://graph.facebook.com/v21.0/"

typedef struct hu_facebook_ctx {
    char _unused;
} hu_facebook_ctx_t;

static hu_error_t facebook_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out)
        return HU_ERR_INVALID_ARGUMENT;
    const char *op = hu_json_get_string(args, "operation");
    const char *page_id = hu_json_get_string(args, "page_id");
    if (!op || strlen(op) == 0 || !page_id || strlen(page_id) == 0) {
        *out = hu_tool_result_fail("Missing 'operation' or 'page_id'", 30);
        return HU_OK;
    }

#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "{\"success\":true,\"id\":\"mock_123\"}", 33);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 33);
    return HU_OK;
#else
    const char *token = hu_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        *out = hu_tool_result_fail("Missing 'access_token' for Facebook API", 38);
        return HU_OK;
    }

    char auth_buf[512];
    int na = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", token);
    if (na <= 0 || (size_t)na >= sizeof(auth_buf)) {
        *out = hu_tool_result_fail("auth header too long", 20);
        return HU_OK;
    }

    char url_buf[512];
    hu_json_buf_t jbuf;
    hu_http_response_t resp = {0};
    hu_error_t err;

    if (strcmp(op, "post") == 0) {
        const char *message = hu_json_get_string(args, "message");
        if (!message || strlen(message) == 0) {
            *out = hu_tool_result_fail("Missing 'message' for post", 25);
            return HU_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/feed", HU_FB_GRAPH_BASE, page_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = hu_tool_result_fail("out of memory", 12);
            return err;
        }
        err = hu_json_append_key_value(&jbuf, "message", 7, message, strlen(message));
        if (err) {
            hu_json_buf_free(&jbuf);
            *out = hu_tool_result_fail("JSON build failed", 18);
            return err;
        }
        err = hu_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        hu_json_buf_free(&jbuf);
    } else if (strcmp(op, "comment") == 0) {
        const char *post_id = hu_json_get_string(args, "post_id");
        const char *message = hu_json_get_string(args, "message");
        if (!post_id || strlen(post_id) == 0 || !message || strlen(message) == 0) {
            *out = hu_tool_result_fail("Missing 'post_id' or 'message' for comment", 41);
            return HU_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/comments", HU_FB_GRAPH_BASE, post_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = hu_tool_result_fail("out of memory", 12);
            return err;
        }
        err = hu_json_append_key_value(&jbuf, "message", 7, message, strlen(message));
        if (err) {
            hu_json_buf_free(&jbuf);
            *out = hu_tool_result_fail("JSON build failed", 18);
            return err;
        }
        err = hu_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        hu_json_buf_free(&jbuf);
    } else if (strcmp(op, "list_posts") == 0) {
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/feed?fields=id,message,created_time",
                         HU_FB_GRAPH_BASE, page_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    } else {
        *out = hu_tool_result_fail("Unknown operation", 17);
        return HU_OK;
    }

    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_fail("Facebook API request failed", 28);
        return HU_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        char *rbody = resp.body && resp.body_len > 0 ? hu_strndup(alloc, resp.body, resp.body_len)
                                                     : hu_strndup(alloc, "API error", 9);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        if (rbody)
            *out = hu_tool_result_fail_owned(rbody, strlen(rbody));
        else
            *out = hu_tool_result_fail("API error", 9);
        return HU_OK;
    }
    char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    if (rbody)
        *out = hu_tool_result_ok_owned(rbody, strlen(rbody));
    else
        *out = hu_tool_result_ok("{}", 2);
    return HU_OK;
#endif
}

static const char *facebook_name(void *ctx) {
    (void)ctx;
    return HU_FACEBOOK_NAME;
}
static const char *facebook_description(void *ctx) {
    (void)ctx;
    return HU_FACEBOOK_DESC;
}
static const char *facebook_parameters_json(void *ctx) {
    (void)ctx;
    return HU_FACEBOOK_PARAMS;
}
static void facebook_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_facebook_ctx_t));
}

static const hu_tool_vtable_t facebook_vtable = {
    .execute = facebook_execute,
    .name = facebook_name,
    .description = facebook_description,
    .parameters_json = facebook_parameters_json,
    .deinit = facebook_deinit,
};

hu_error_t hu_facebook_tool_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_facebook_ctx_t *c = (hu_facebook_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &facebook_vtable;
    return HU_OK;
}
