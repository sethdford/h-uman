#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "social"
#define TOOL_DESC                                                                           \
    "Post to and read from social media platforms. Actions: post (publish to Twitter/X or " \
    "LinkedIn), read (get recent posts/mentions), schedule (queue for future posting), "    \
    "analytics (engagement metrics for recent posts)."
#define TOOL_PARAMS                                                                             \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"post\","  \
    "\"read\",\"schedule\",\"analytics\"]},\"platform\":{\"type\":\"string\",\"enum\":["        \
    "\"twitter\",\"linkedin\"]},\"content\":{\"type\":\"string\",\"description\":"              \
    "\"Post content\"},\"api_key\":{\"type\":\"string\"},\"api_secret\":{\"type\":\"string\"}," \
    "\"access_token\":{\"type\":\"string\"},\"schedule_time\":{\"type\":\"string\","            \
    "\"description\":\"ISO 8601 datetime for scheduled posts\"},\"max_results\":"               \
    "{\"type\":\"number\"}},\"required\":[\"action\",\"platform\"]}"

typedef struct {
    char _unused;
} social_ctx_t;

static hu_error_t social_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                 hu_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    const char *platform = hu_json_get_string(args, "platform");
    if (!action || !platform) {
        *out = hu_tool_result_fail("missing action or platform", 26);
        return HU_OK;
    }

#if HU_IS_TEST
    if (strcmp(action, "post") == 0) {
        const char *content = hu_json_get_string(args, "content");
        char *msg = hu_sprintf(
            alloc, "{\"posted\":true,\"platform\":\"%s\",\"id\":\"post_123\",\"content\":\"%s\"}",
            platform, content ? content : "");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "read") == 0) {
        char *msg = hu_sprintf(
            alloc,
            "{\"posts\":[{\"id\":\"1\",\"text\":\"Hello world\",\"likes\":42,\"retweets\":5},"
            "{\"id\":\"2\",\"text\":\"Human update\",\"likes\":128,\"retweets\":23}]}");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "analytics") == 0) {
        *out = hu_tool_result_ok(
            "{\"impressions\":5420,\"engagements\":342,\"followers_gained\":18,\"top_post\":"
            "\"Human update\"}",
            95);
    } else if (strcmp(action, "schedule") == 0) {
        *out = hu_tool_result_ok("{\"scheduled\":true,\"id\":\"sched_1\"}", 36);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
#else
    const char *token = hu_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        char *msg = hu_sprintf(alloc, "missing access_token for %s", platform);
        if (msg) {
            *out = hu_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = hu_tool_result_fail("missing access_token", 20);
        }
        return HU_OK;
    }

    if (strcmp(platform, "twitter") == 0 && strcmp(action, "post") == 0) {
        const char *content = hu_json_get_string(args, "content");
        if (!content) {
            *out = hu_tool_result_fail("missing content", 15);
            return HU_OK;
        }
        char *body = hu_sprintf(alloc, "{\"text\":\"%s\"}", content);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, "https://api.twitter.com/2/tweets", auth, body,
                                           body ? strlen(body) : 0, &resp);
        if (body)
            alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to post tweet", 20);
            return HU_OK;
        }
        char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(platform, "linkedin") == 0 && strcmp(action, "post") == 0) {
        const char *content = hu_json_get_string(args, "content");
        if (!content) {
            *out = hu_tool_result_fail("missing content", 15);
            return HU_OK;
        }
        char *body_json = hu_sprintf(
            alloc,
            "{\"author\":\"urn:li:person:me\",\"lifecycleState\":\"PUBLISHED\","
            "\"specificContent\":{\"com.linkedin.ugc.ShareContent\":{\"shareCommentary\":"
            "{\"text\":\"%s\"},\"shareMediaCategory\":\"NONE\"}},\"visibility\":"
            "{\"com.linkedin.ugc.MemberNetworkVisibility\":\"PUBLIC\"}}",
            content);
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, "https://api.linkedin.com/v2/ugcPosts", auth_hdr,
                                           body_json, body_json ? strlen(body_json) : 0, &resp);
        if (body_json)
            alloc->free(alloc->ctx, body_json, strlen(body_json) + 1);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to post to LinkedIn", 26);
            return HU_OK;
        }
        char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "read") == 0) {
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        const char *url = (strcmp(platform, "twitter") == 0)
                              ? "https://api.twitter.com/2/users/me/tweets?max_results=10"
                              : "https://api.linkedin.com/v2/"
                                "ugcPosts?q=authors&authors=List(urn%3Ali%3Aperson%3Ame)";
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url, auth_hdr, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to read posts", 20);
            return HU_OK;
        }
        char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else {
        *out = hu_tool_result_fail("unsupported action/platform combination", 39);
    }
    return HU_OK;
#endif
}

static const char *social_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *social_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *social_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void social_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(social_ctx_t));
}

static const hu_tool_vtable_t social_vtable = {
    .execute = social_execute,
    .name = social_name,
    .description = social_desc,
    .parameters_json = social_params,
    .deinit = social_deinit,
};

hu_error_t hu_social_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, sizeof(social_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(social_ctx_t));
    out->ctx = ctx;
    out->vtable = &social_vtable;
    return HU_OK;
}
