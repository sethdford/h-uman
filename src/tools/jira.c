#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "jira"
#define TOOL_DESC                                                                             \
    "Manage Jira/Linear issues. Actions: list (search issues with JQL), create (new issue), " \
    "update (change status/assignee/fields), comment (add comment), get (issue details). "    \
    "Requires base_url and api_token."
#define TOOL_PARAMS                                                                            \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\"," \
    "\"create\",\"update\",\"comment\",\"get\"]},\"base_url\":{\"type\":\"string\","           \
    "\"description\":\"Jira instance URL (e.g. https://org.atlassian.net)\"},\"api_token\":"   \
    "{\"type\":\"string\"},\"email\":{\"type\":\"string\",\"description\":\"Jira account "     \
    "email\"},\"project\":{\"type\":\"string\"},\"issue_key\":{\"type\":\"string\"},"          \
    "\"summary\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},"                 \
    "\"issue_type\":{\"type\":\"string\",\"description\":\"Task, Bug, Story, Epic\"},"         \
    "\"status\":{\"type\":\"string\"},\"assignee\":{\"type\":\"string\"},\"jql\":"             \
    "{\"type\":\"string\",\"description\":\"JQL query for list action\"},\"comment\":"         \
    "{\"type\":\"string\"}},\"required\":[\"action\"]}"

typedef struct {
    char _unused;
} jira_ctx_t;

static hu_error_t jira_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                               hu_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

#if HU_IS_TEST
    if (strcmp(action, "list") == 0) {
        const char *resp =
            "{\"issues\":[{\"key\":\"PROJ-1\",\"summary\":\"Fix login bug\",\"status\":\"Open\","
            "\"assignee\":\"alice\"},{\"key\":\"PROJ-2\",\"summary\":\"Add dark mode\","
            "\"status\":\"In Progress\",\"assignee\":\"bob\"}]}";
        *out = hu_tool_result_ok(resp, strlen(resp));
    } else if (strcmp(action, "create") == 0) {
        const char *summary = hu_json_get_string(args, "summary");
        char *msg = hu_sprintf(alloc, "{\"created\":true,\"key\":\"PROJ-42\",\"summary\":\"%s\"}",
                               summary ? summary : "Untitled");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "get") == 0) {
        const char *key = hu_json_get_string(args, "issue_key");
        char *msg = hu_sprintf(alloc,
                               "{\"key\":\"%s\",\"summary\":\"Test issue\",\"status\":\"Open\","
                               "\"description\":\"Test description\",\"assignee\":\"alice\"}",
                               key ? key : "PROJ-1");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "comment") == 0) {
        *out = hu_tool_result_ok("{\"added\":true}", 15);
    } else if (strcmp(action, "update") == 0) {
        *out = hu_tool_result_ok("{\"updated\":true}", 17);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
#else
    const char *base_url = hu_json_get_string(args, "base_url");
    const char *api_token = hu_json_get_string(args, "api_token");
    const char *email = hu_json_get_string(args, "email");
    if (!base_url || strlen(base_url) == 0 || !api_token || strlen(api_token) == 0) {
        *out = hu_tool_result_fail("missing base_url or api_token", 29);
        return HU_OK;
    }
    if (strncmp(base_url, "https://", 8) != 0) {
        *out = hu_tool_result_fail("base_url must use https://", 25);
        return HU_OK;
    }
    if (strlen(base_url) > 400) {
        *out = hu_tool_result_fail("base_url too long", 17);
        return HU_OK;
    }
    char auth[512];
    int auth_n;
    if (email)
        auth_n = snprintf(auth, sizeof(auth), "Basic %s:%s", email, api_token);
    else
        auth_n = snprintf(auth, sizeof(auth), "Bearer %s", api_token);
    if (auth_n < 0 || (size_t)auth_n >= sizeof(auth)) {
        *out = hu_tool_result_fail("auth header too long", 20);
        return HU_OK;
    }

    if (strcmp(action, "list") == 0) {
        const char *jql = hu_json_get_string(args, "jql");
        if (!jql)
            jql = "ORDER BY updated DESC";
        if (strlen(jql) > 512) {
            *out = hu_tool_result_fail("jql too long", 12);
            return HU_OK;
        }
        char url[2048];
        int url_n =
            snprintf(url, sizeof(url), "%s/rest/api/3/search?jql=%s&maxResults=20", base_url, jql);
        if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url, auth, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to list issues", 21);
            return HU_OK;
        }
        char *body = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "create") == 0) {
        const char *project = hu_json_get_string(args, "project");
        const char *summary = hu_json_get_string(args, "summary");
        const char *issue_type = hu_json_get_string(args, "issue_type");
        const char *desc = hu_json_get_string(args, "description");
        if (!project || !summary) {
            *out = hu_tool_result_fail("create needs project and summary", 32);
            return HU_OK;
        }
        char *body = hu_sprintf(
            alloc,
            "{\"fields\":{\"project\":{\"key\":\"%s\"},\"summary\":\"%s\","
            "\"issuetype\":{\"name\":\"%s\"}%s%s%s}}",
            project, summary, issue_type ? issue_type : "Task",
            desc ? ",\"description\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":"
                   "\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\""
                 : "",
            desc ? desc : "", desc ? "\"}]}]}" : "");
        char url[256];
        int url_n = snprintf(url, sizeof(url), "%s/rest/api/3/issue", base_url);
        if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
            if (body)
                alloc->free(alloc->ctx, body, strlen(body) + 1);
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        alloc->free(alloc->ctx, body, body ? strlen(body) + 1 : 0);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to create issue", 22);
            return HU_OK;
        }
        char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "get") == 0) {
        const char *issue_key = hu_json_get_string(args, "issue_key");
        if (!issue_key) {
            *out = hu_tool_result_fail("missing issue_key", 17);
            return HU_OK;
        }
        char url[512];
        int url_n = snprintf(url, sizeof(url), "%s/rest/api/3/issue/%s", base_url, issue_key);
        if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url, auth, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to get issue", 19);
            return HU_OK;
        }
        char *body = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "comment") == 0) {
        const char *issue_key = hu_json_get_string(args, "issue_key");
        const char *comment = hu_json_get_string(args, "comment");
        if (!issue_key || !comment) {
            *out = hu_tool_result_fail("comment needs issue_key and comment", 35);
            return HU_OK;
        }
        char url[512];
        int url_n =
            snprintf(url, sizeof(url), "%s/rest/api/3/issue/%s/comment", base_url, issue_key);
        if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        char *body = hu_sprintf(
            alloc,
            "{\"body\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":\"paragraph\","
            "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}]}}",
            comment);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        if (body)
            alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to add comment", 21);
            return HU_OK;
        }
        char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "update") == 0) {
        const char *issue_key = hu_json_get_string(args, "issue_key");
        if (!issue_key) {
            *out = hu_tool_result_fail("missing issue_key", 17);
            return HU_OK;
        }
        const char *status = hu_json_get_string(args, "status");
        const char *assignee = hu_json_get_string(args, "assignee");
        if (!status && !assignee) {
            *out = hu_tool_result_fail("update needs status or assignee", 31);
            return HU_OK;
        }
        char *body =
            hu_sprintf(alloc, "{\"fields\":{%s%s%s%s%s%s%s}}",
                       assignee ? "\"assignee\":{\"accountId\":\"" : "", assignee ? assignee : "",
                       assignee ? "\"}" : "", (assignee && status) ? "," : "",
                       status ? "\"labels\":[\"" : "", status ? status : "", status ? "\"]" : "");
        if (!body) {
            *out = hu_tool_result_fail("alloc failed", 12);
            return HU_OK;
        }
        char url[512];
        int url_n = snprintf(url, sizeof(url), "%s/rest/api/3/issue/%s", base_url, issue_key);
        if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
            alloc->free(alloc->ctx, body, strlen(body) + 1);
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, url, auth, body, strlen(body), &resp);
        alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to update issue", 22);
            return HU_OK;
        }
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok("{\"updated\":true}", 17);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
#endif
}

static const char *jira_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *jira_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *jira_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void jira_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(jira_ctx_t));
}

static const hu_tool_vtable_t jira_vtable = {
    .execute = jira_execute,
    .name = jira_name,
    .description = jira_desc,
    .parameters_json = jira_params,
    .deinit = jira_deinit,
};

hu_error_t hu_jira_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, sizeof(jira_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(jira_ctx_t));
    out->ctx = ctx;
    out->vtable = &jira_vtable;
    return HU_OK;
}
