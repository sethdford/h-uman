/*
 * Instagram Posts tool — publish photos, list media, comment.
 */
#include "human/tools/instagram.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_INSTAGRAM_NAME "instagram_posts"
#define HU_INSTAGRAM_DESC "Publish photos, list media, and comment on Instagram posts"
#define HU_INSTAGRAM_PARAMS                                                                        \
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":["           \
    "\"publish_photo\",\"list_media\",\"comment\"]},\"account_id\":{\"type\":\"string\"},"         \
    "\"image_url\":{\"type\":\"string\"},\"caption\":{\"type\":\"string\"},\"media_id\":{"         \
    "\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"access_token\":{\"type\":\"string\"" \
    "}},\"required\":[\"operation\",\"account_id\"]}"

#define HU_IG_GRAPH_BASE "https://graph.facebook.com/v21.0/"

typedef struct hu_instagram_ctx {
    char _unused;
} hu_instagram_ctx_t;

static hu_error_t instagram_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out)
        return HU_ERR_INVALID_ARGUMENT;
    const char *op = hu_json_get_string(args, "operation");
    const char *account_id = hu_json_get_string(args, "account_id");
    if (!op || strlen(op) == 0 || !account_id || strlen(account_id) == 0) {
        *out = hu_tool_result_fail("Missing 'operation' or 'account_id'", 29);
        return HU_OK;
    }

#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "{\"success\":true,\"id\":\"mock_media_123\"}", 33);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 33);
    return HU_OK;
#else
    const char *token = hu_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        *out = hu_tool_result_fail("Missing 'access_token' for Instagram API", 40);
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

    if (strcmp(op, "publish_photo") == 0) {
        const char *image_url = hu_json_get_string(args, "image_url");
        if (!image_url || strlen(image_url) == 0) {
            *out = hu_tool_result_fail("Missing 'image_url' for publish_photo", 37);
            return HU_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/media", HU_IG_GRAPH_BASE, account_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = hu_tool_result_fail("out of memory", 12);
            return err;
        }
        err = hu_json_append_key_value(&jbuf, "image_url", 9, image_url, strlen(image_url));
        if (err)
            goto jfail;
        {
            const char *caption = hu_json_get_string(args, "caption");
            if (caption && strlen(caption) > 0)
                err = hu_json_append_key_value(&jbuf, "caption", 7, caption, strlen(caption));
        }
        if (err)
            goto jfail;
        err = hu_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        hu_json_buf_free(&jbuf);
        if (err != HU_OK)
            goto resp_fail;
        if (resp.status_code < 200 || resp.status_code >= 300)
            goto resp_err;
        hu_json_value_t *parsed = NULL;
        err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !parsed) {
            *out = hu_tool_result_fail("Invalid container response", 26);
            return HU_OK;
        }
        const char *creation_id = hu_json_get_string(parsed, "id");
        if (!creation_id) {
            hu_json_free(alloc, parsed);
            *out = hu_tool_result_fail("No container id in response", 28);
            return HU_OK;
        }
        n = snprintf(url_buf, sizeof(url_buf), "%s%s/media_publish", HU_IG_GRAPH_BASE, account_id);
        hu_json_free(alloc, parsed);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_json_buf_init(&jbuf, alloc);
        if (err) {
            *out = hu_tool_result_fail("out of memory", 12);
            return err;
        }
        err = hu_json_append_key_value(&jbuf, "creation_id", 10, creation_id, strlen(creation_id));
        if (err) {
            hu_json_buf_free(&jbuf);
            *out = hu_tool_result_fail("JSON build failed", 18);
            return err;
        }
        err = hu_http_post_json(alloc, url_buf, auth_buf, jbuf.ptr, jbuf.len, &resp);
        hu_json_buf_free(&jbuf);
        if (err != HU_OK)
            goto resp_fail;
        if (resp.status_code < 200 || resp.status_code >= 300)
            goto resp_err;
    } else if (strcmp(op, "list_media") == 0) {
        int n =
            snprintf(url_buf, sizeof(url_buf), "%s%s/media?fields=id,caption,media_type,media_url",
                     HU_IG_GRAPH_BASE, account_id);
        if (n < 0 || (size_t)n >= sizeof(url_buf)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_http_get(alloc, url_buf, auth_buf, &resp);
    } else if (strcmp(op, "comment") == 0) {
        const char *media_id = hu_json_get_string(args, "media_id");
        const char *message = hu_json_get_string(args, "message");
        if (!media_id || strlen(media_id) == 0 || !message || strlen(message) == 0) {
            *out = hu_tool_result_fail("Missing 'media_id' or 'message' for comment", 41);
            return HU_OK;
        }
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s/comments", HU_IG_GRAPH_BASE, media_id);
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
    } else {
        *out = hu_tool_result_fail("Unknown operation", 17);
        return HU_OK;
    }

    if (err != HU_OK) {
    resp_fail:
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_fail("Instagram API request failed", 29);
        return HU_OK;
    }
resp_err:
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

jfail:
    hu_json_buf_free(&jbuf);
    *out = hu_tool_result_fail("JSON build failed", 18);
    return err;
#endif
}

static const char *instagram_name(void *ctx) {
    (void)ctx;
    return HU_INSTAGRAM_NAME;
}
static const char *instagram_description(void *ctx) {
    (void)ctx;
    return HU_INSTAGRAM_DESC;
}
static const char *instagram_parameters_json(void *ctx) {
    (void)ctx;
    return HU_INSTAGRAM_PARAMS;
}
static void instagram_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_instagram_ctx_t));
}

static const hu_tool_vtable_t instagram_vtable = {
    .execute = instagram_execute,
    .name = instagram_name,
    .description = instagram_description,
    .parameters_json = instagram_parameters_json,
    .deinit = instagram_deinit,
};

hu_error_t hu_instagram_tool_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_instagram_ctx_t *c = (hu_instagram_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &instagram_vtable;
    return HU_OK;
}
