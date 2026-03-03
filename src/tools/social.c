#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
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

static sc_error_t social_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                 sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }
    const char *action = sc_json_get_string(args, "action");
    const char *platform = sc_json_get_string(args, "platform");
    if (!action || !platform) {
        *out = sc_tool_result_fail("missing action or platform", 26);
        return SC_OK;
    }

#if SC_IS_TEST
    if (strcmp(action, "post") == 0) {
        const char *content = sc_json_get_string(args, "content");
        char *msg = sc_sprintf(
            alloc, "{\"posted\":true,\"platform\":\"%s\",\"id\":\"post_123\",\"content\":\"%s\"}",
            platform, content ? content : "");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "read") == 0) {
        char *msg = sc_sprintf(
            alloc,
            "{\"posts\":[{\"id\":\"1\",\"text\":\"Hello world\",\"likes\":42,\"retweets\":5},"
            "{\"id\":\"2\",\"text\":\"SeaClaw update\",\"likes\":128,\"retweets\":23}]}");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "analytics") == 0) {
        *out = sc_tool_result_ok(
            "{\"impressions\":5420,\"engagements\":342,\"followers_gained\":18,\"top_post\":"
            "\"SeaClaw update\"}",
            95);
    } else if (strcmp(action, "schedule") == 0) {
        *out = sc_tool_result_ok("{\"scheduled\":true,\"id\":\"sched_1\"}", 36);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
#else
    const char *token = sc_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        char *msg = sc_sprintf(alloc, "missing access_token for %s", platform);
        *out = sc_tool_result_fail(msg, msg ? strlen(msg) : 0);
        if (msg)
            alloc->free(alloc->ctx, msg, strlen(msg) + 1);
        return SC_OK;
    }

    if (strcmp(platform, "twitter") == 0 && strcmp(action, "post") == 0) {
        const char *content = sc_json_get_string(args, "content");
        if (!content) {
            *out = sc_tool_result_fail("missing content", 15);
            return SC_OK;
        }
        char *body = sc_sprintf(alloc, "{\"text\":\"%s\"}", content);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, "https://api.twitter.com/2/tweets", auth, body,
                                           body ? strlen(body) : 0, &resp);
        if (body)
            alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to post tweet", 20);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(platform, "linkedin") == 0 && strcmp(action, "post") == 0) {
        const char *content = sc_json_get_string(args, "content");
        if (!content) { *out = sc_tool_result_fail("missing content", 15); return SC_OK; }
        char *body_json = sc_sprintf(alloc,
            "{\"author\":\"urn:li:person:me\",\"lifecycleState\":\"PUBLISHED\","
            "\"specificContent\":{\"com.linkedin.ugc.ShareContent\":{\"shareCommentary\":"
            "{\"text\":\"%s\"},\"shareMediaCategory\":\"NONE\"}},\"visibility\":"
            "{\"com.linkedin.ugc.MemberNetworkVisibility\":\"PUBLIC\"}}", content);
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, "https://api.linkedin.com/v2/ugcPosts",
                                           auth_hdr, body_json, body_json ? strlen(body_json) : 0, &resp);
        if (body_json) alloc->free(alloc->ctx, body_json, strlen(body_json) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to post to LinkedIn", 26);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "read") == 0) {
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        const char *url = (strcmp(platform, "twitter") == 0)
            ? "https://api.twitter.com/2/users/me/tweets?max_results=10"
            : "https://api.linkedin.com/v2/ugcPosts?q=authors&authors=List(urn%3Ali%3Aperson%3Ame)";
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, url, auth_hdr, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to read posts", 20);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else {
        *out = sc_tool_result_fail("unsupported action/platform combination", 39);
    }
    return SC_OK;
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
static void social_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t social_vtable = {
    .execute = social_execute,
    .name = social_name,
    .description = social_desc,
    .parameters_json = social_params,
    .deinit = social_deinit,
};

sc_error_t sc_social_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(social_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &social_vtable;
    return SC_OK;
}
