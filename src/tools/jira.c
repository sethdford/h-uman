#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
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
    int placeholder;
} jira_ctx_t;

static sc_error_t jira_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                               sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action) {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }

#if SC_IS_TEST
    if (strcmp(action, "list") == 0) {
        const char *resp =
            "{\"issues\":[{\"key\":\"PROJ-1\",\"summary\":\"Fix login bug\",\"status\":\"Open\","
            "\"assignee\":\"alice\"},{\"key\":\"PROJ-2\",\"summary\":\"Add dark mode\","
            "\"status\":\"In Progress\",\"assignee\":\"bob\"}]}";
        *out = sc_tool_result_ok(resp, strlen(resp));
    } else if (strcmp(action, "create") == 0) {
        const char *summary = sc_json_get_string(args, "summary");
        char *msg = sc_sprintf(alloc, "{\"created\":true,\"key\":\"PROJ-42\",\"summary\":\"%s\"}",
                               summary ? summary : "Untitled");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "get") == 0) {
        const char *key = sc_json_get_string(args, "issue_key");
        char *msg = sc_sprintf(alloc,
                               "{\"key\":\"%s\",\"summary\":\"Test issue\",\"status\":\"Open\","
                               "\"description\":\"Test description\",\"assignee\":\"alice\"}",
                               key ? key : "PROJ-1");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "comment") == 0) {
        *out = sc_tool_result_ok("{\"added\":true}", 15);
    } else if (strcmp(action, "update") == 0) {
        *out = sc_tool_result_ok("{\"updated\":true}", 17);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
#else
    const char *base_url = sc_json_get_string(args, "base_url");
    const char *api_token = sc_json_get_string(args, "api_token");
    const char *email = sc_json_get_string(args, "email");
    if (!base_url || strlen(base_url) == 0 || !api_token || strlen(api_token) == 0) {
        *out = sc_tool_result_fail("missing base_url or api_token", 29);
        return SC_OK;
    }
    if (strncmp(base_url, "https://", 8) != 0) {
        *out = sc_tool_result_fail("base_url must use https://", 25);
        return SC_OK;
    }
    if (strlen(base_url) > 400) {
        *out = sc_tool_result_fail("base_url too long", 17);
        return SC_OK;
    }
    char auth[512];
    if (email)
        snprintf(auth, sizeof(auth), "Basic %s:%s", email, api_token);
    else
        snprintf(auth, sizeof(auth), "Bearer %s", api_token);

    if (strcmp(action, "list") == 0) {
        const char *jql = sc_json_get_string(args, "jql");
        if (!jql)
            jql = "ORDER BY updated DESC";
        if (strlen(jql) > 512) {
            *out = sc_tool_result_fail("jql too long", 12);
            return SC_OK;
        }
        char url[1024];
        snprintf(url, sizeof(url), "%s/rest/api/3/search?jql=%s&maxResults=20", base_url, jql);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, url, auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to list issues", 21);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "create") == 0) {
        const char *project = sc_json_get_string(args, "project");
        const char *summary = sc_json_get_string(args, "summary");
        const char *issue_type = sc_json_get_string(args, "issue_type");
        const char *desc = sc_json_get_string(args, "description");
        if (!project || !summary) {
            *out = sc_tool_result_fail("create needs project and summary", 32);
            return SC_OK;
        }
        char *body = sc_sprintf(
            alloc,
            "{\"fields\":{\"project\":{\"key\":\"%s\"},\"summary\":\"%s\","
            "\"issuetype\":{\"name\":\"%s\"}%s%s%s}}",
            project, summary, issue_type ? issue_type : "Task",
            desc ? ",\"description\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":"
                   "\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\""
                 : "",
            desc ? desc : "", desc ? "\"}]}]}" : "");
        char url[256];
        snprintf(url, sizeof(url), "%s/rest/api/3/issue", base_url);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        alloc->free(alloc->ctx, body, body ? strlen(body) + 1 : 0);
        if (err != SC_OK) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to create issue", 22);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "get") == 0) {
        const char *issue_key = sc_json_get_string(args, "issue_key");
        if (!issue_key) { *out = sc_tool_result_fail("missing issue_key", 17); return SC_OK; }
        char url[512];
        snprintf(url, sizeof(url), "%s/rest/api/3/issue/%s", base_url, issue_key);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, url, auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to get issue", 19);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "comment") == 0) {
        const char *issue_key = sc_json_get_string(args, "issue_key");
        const char *comment = sc_json_get_string(args, "comment");
        if (!issue_key || !comment) {
            *out = sc_tool_result_fail("comment needs issue_key and comment", 35);
            return SC_OK;
        }
        char url[512];
        snprintf(url, sizeof(url), "%s/rest/api/3/issue/%s/comment", base_url, issue_key);
        char *body = sc_sprintf(alloc,
            "{\"body\":{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":\"paragraph\","
            "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}]}}", comment);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        if (body) alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to add comment", 21);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "update") == 0) {
        const char *issue_key = sc_json_get_string(args, "issue_key");
        if (!issue_key) { *out = sc_tool_result_fail("missing issue_key", 17); return SC_OK; }
        const char *status = sc_json_get_string(args, "status");
        const char *assignee = sc_json_get_string(args, "assignee");
        char *body = sc_sprintf(alloc, "{\"fields\":{%s%s%s%s%s%s}}",
            assignee ? "\"assignee\":{\"accountId\":\"" : "",
            assignee ? assignee : "", assignee ? "\"}" : "",
            (assignee && status) ? "," : "",
            status ? "\"status\":{\"name\":\"" : "",
            status ? status : "");
        if (status) {
            size_t bl = body ? strlen(body) : 0;
            if (body) alloc->free(alloc->ctx, body, bl + 1);
            body = sc_sprintf(alloc, "{\"fields\":{%s%s%s}}",
                assignee ? "\"assignee\":{\"accountId\":\"" : "",
                assignee ? assignee : "", assignee ? "\"}" : "");
        }
        char url[512];
        snprintf(url, sizeof(url), "%s/rest/api/3/issue/%s", base_url, issue_key);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        if (body) alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to update issue", 22);
            return SC_OK;
        }
        if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok("{\"updated\":true}", 17);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
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
static void jira_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t jira_vtable = {
    .execute = jira_execute,
    .name = jira_name,
    .description = jira_desc,
    .parameters_json = jira_params,
    .deinit = jira_deinit,
};

sc_error_t sc_jira_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(jira_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &jira_vtable;
    return SC_OK;
}
