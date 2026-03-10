#include "human/paperclip/client.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *env_or_null(hu_allocator_t *alloc, const char *name) {
    const char *val = getenv(name);
    if (!val || !*val)
        return NULL;
    return hu_strdup(alloc, val);
}

hu_error_t hu_paperclip_client_init(hu_paperclip_client_t *client, hu_allocator_t *alloc) {
    if (!client || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    memset(client, 0, sizeof(*client));
    client->alloc = alloc;

    client->api_url = env_or_null(alloc, "PAPERCLIP_API_URL");
    client->agent_id = env_or_null(alloc, "PAPERCLIP_AGENT_ID");
    client->company_id = env_or_null(alloc, "PAPERCLIP_COMPANY_ID");
    client->api_key = env_or_null(alloc, "PAPERCLIP_API_KEY");
    client->run_id = env_or_null(alloc, "PAPERCLIP_RUN_ID");
    client->task_id = env_or_null(alloc, "PAPERCLIP_TASK_ID");
    client->wake_reason = env_or_null(alloc, "PAPERCLIP_WAKE_REASON");

    if (!client->api_url || !client->agent_id) {
        hu_paperclip_client_deinit(client);
        return HU_ERR_INVALID_ARGUMENT;
    }
    return HU_OK;
}

hu_error_t hu_paperclip_client_init_from_config(hu_paperclip_client_t *client,
                                                 hu_allocator_t *alloc, const char *api_url,
                                                 const char *agent_id, const char *company_id) {
    if (!client || !alloc || !api_url || !agent_id)
        return HU_ERR_INVALID_ARGUMENT;
    memset(client, 0, sizeof(*client));
    client->alloc = alloc;
    client->api_url = hu_strdup(alloc, api_url);
    client->agent_id = hu_strdup(alloc, agent_id);
    if (company_id)
        client->company_id = hu_strdup(alloc, company_id);
    client->api_key = env_or_null(alloc, "PAPERCLIP_API_KEY");
    client->run_id = env_or_null(alloc, "PAPERCLIP_RUN_ID");
    client->task_id = env_or_null(alloc, "PAPERCLIP_TASK_ID");
    client->wake_reason = env_or_null(alloc, "PAPERCLIP_WAKE_REASON");
    return HU_OK;
}

static void free_str(hu_allocator_t *a, char **s) {
    if (*s) {
        a->free(a->ctx, *s, strlen(*s) + 1);
        *s = NULL;
    }
}

void hu_paperclip_client_deinit(hu_paperclip_client_t *client) {
    if (!client || !client->alloc)
        return;
    hu_allocator_t *a = client->alloc;
    free_str(a, &client->api_url);
    free_str(a, &client->agent_id);
    free_str(a, &client->company_id);
    free_str(a, &client->api_key);
    free_str(a, &client->run_id);
    free_str(a, &client->task_id);
    free_str(a, &client->wake_reason);
}

static char *build_auth_header(hu_allocator_t *alloc, const hu_paperclip_client_t *client) {
    if (!client->api_key)
        return NULL;
    size_t key_len = strlen(client->api_key);
    size_t hdr_len = 7 + key_len + 1;
    char *hdr = (char *)alloc->alloc(alloc->ctx, hdr_len);
    if (!hdr)
        return NULL;
    snprintf(hdr, hdr_len, "Bearer %s", client->api_key);
    return hdr;
}

static hu_paperclip_task_t parse_task(hu_allocator_t *alloc, const hu_json_value_t *obj) {
    hu_paperclip_task_t t = {0};
    if (!obj || obj->type != HU_JSON_OBJECT)
        return t;
    const char *s;
    s = hu_json_get_string(obj, "id");
    if (s) t.id = hu_strdup(alloc, s);
    s = hu_json_get_string(obj, "title");
    if (s) t.title = hu_strdup(alloc, s);
    s = hu_json_get_string(obj, "description");
    if (s) t.description = hu_strdup(alloc, s);
    s = hu_json_get_string(obj, "status");
    if (s) t.status = hu_strdup(alloc, s);
    s = hu_json_get_string(obj, "priority");
    if (s) t.priority = hu_strdup(alloc, s);

    hu_json_value_t *proj = hu_json_object_get((hu_json_value_t *)obj, "project");
    if (proj && proj->type == HU_JSON_OBJECT) {
        s = hu_json_get_string(proj, "name");
        if (s) t.project_name = hu_strdup(alloc, s);
    }
    hu_json_value_t *goal = hu_json_object_get((hu_json_value_t *)obj, "goal");
    if (goal && goal->type == HU_JSON_OBJECT) {
        s = hu_json_get_string(goal, "title");
        if (s) t.goal_title = hu_strdup(alloc, s);
    }
    return t;
}

void hu_paperclip_task_free(hu_allocator_t *alloc, hu_paperclip_task_t *task) {
    if (!task || !alloc)
        return;
    free_str(alloc, &task->id);
    free_str(alloc, &task->title);
    free_str(alloc, &task->description);
    free_str(alloc, &task->status);
    free_str(alloc, &task->priority);
    free_str(alloc, &task->project_name);
    free_str(alloc, &task->goal_title);
}

void hu_paperclip_task_list_free(hu_allocator_t *alloc, hu_paperclip_task_list_t *list) {
    if (!list || !alloc)
        return;
    for (size_t i = 0; i < list->count; i++)
        hu_paperclip_task_free(alloc, &list->tasks[i]);
    if (list->tasks)
        alloc->free(alloc->ctx, list->tasks,
                    list->count * sizeof(hu_paperclip_task_t));
    list->tasks = NULL;
    list->count = 0;
}

void hu_paperclip_comment_list_free(hu_allocator_t *alloc, hu_paperclip_comment_list_t *list) {
    if (!list || !alloc)
        return;
    for (size_t i = 0; i < list->count; i++) {
        free_str(alloc, &list->comments[i].id);
        free_str(alloc, &list->comments[i].body);
        free_str(alloc, &list->comments[i].author_name);
        free_str(alloc, &list->comments[i].created_at);
    }
    if (list->comments)
        alloc->free(alloc->ctx, list->comments,
                    list->count * sizeof(hu_paperclip_comment_t));
    list->comments = NULL;
    list->count = 0;
}

hu_error_t hu_paperclip_list_tasks(hu_paperclip_client_t *client,
                                    hu_paperclip_task_list_t *out) {
    if (!client || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char url[512];
    if (client->company_id) {
        snprintf(url, sizeof(url),
                 "%s/companies/%s/issues?assigneeAgentId=%s&status=todo,in_progress,blocked",
                 client->api_url, client->company_id, client->agent_id);
    } else {
        snprintf(url, sizeof(url), "%s/issues?assigneeAgentId=%s&status=todo,in_progress,blocked",
                 client->api_url, client->agent_id);
    }

#ifdef HU_IS_TEST
    (void)url;
    return HU_OK;
#endif

    char *auth = build_auth_header(client->alloc, client);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(client->alloc, url, auth, &resp);
    if (auth)
        client->alloc->free(client->alloc->ctx, auth, strlen(auth) + 1);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(client->alloc, &resp);
        return HU_ERR_IO;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(client->alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(client->alloc, &resp);
    if (err != HU_OK)
        return err;

    hu_json_value_t *items = hu_json_object_get(root, "items");
    if (!items)
        items = root;
    if (items->type == HU_JSON_ARRAY && items->data.array.len > 0) {
        size_t n = items->data.array.len;
        out->tasks = (hu_paperclip_task_t *)client->alloc->alloc(
            client->alloc->ctx, n * sizeof(hu_paperclip_task_t));
        if (!out->tasks) {
            hu_json_free(client->alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(out->tasks, 0, n * sizeof(hu_paperclip_task_t));
        for (size_t i = 0; i < n; i++)
            out->tasks[i] = parse_task(client->alloc, items->data.array.items[i]);
        out->count = n;
    }
    hu_json_free(client->alloc, root);
    return HU_OK;
}

hu_error_t hu_paperclip_get_task(hu_paperclip_client_t *client, const char *task_id,
                                  hu_paperclip_task_t *out) {
    if (!client || !task_id || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url), "%s/issues/%s", client->api_url, task_id);

#ifdef HU_IS_TEST
    (void)url;
    return HU_OK;
#endif

    char *auth = build_auth_header(client->alloc, client);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(client->alloc, url, auth, &resp);
    if (auth)
        client->alloc->free(client->alloc->ctx, auth, strlen(auth) + 1);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(client->alloc, &resp);
        return HU_ERR_IO;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(client->alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(client->alloc, &resp);
    if (err != HU_OK)
        return err;

    *out = parse_task(client->alloc, root);
    hu_json_free(client->alloc, root);
    return HU_OK;
}

hu_error_t hu_paperclip_checkout_task(hu_paperclip_client_t *client, const char *task_id) {
    if (!client || !task_id)
        return HU_ERR_INVALID_ARGUMENT;

    char url[512];
    snprintf(url, sizeof(url), "%s/issues/%s/checkout", client->api_url, task_id);

#ifdef HU_IS_TEST
    (void)url;
    return HU_OK;
#endif

    char body[256];
    int blen = snprintf(body, sizeof(body),
                        "{\"agentId\":\"%s\",\"expectedStatuses\":[\"todo\",\"backlog\",\"blocked\"]}",
                        client->agent_id);

    char *auth = build_auth_header(client->alloc, client);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(client->alloc, url, auth, body, (size_t)blen, &resp);
    if (auth)
        client->alloc->free(client->alloc->ctx, auth, strlen(auth) + 1);
    if (err != HU_OK)
        return err;

    long code = resp.status_code;
    hu_http_response_free(client->alloc, &resp);

    if (code == 409)
        return HU_ERR_ALREADY_EXISTS;
    if (code < 200 || code >= 300)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_paperclip_update_task(hu_paperclip_client_t *client, const char *task_id,
                                     const char *status) {
    if (!client || !task_id || !status)
        return HU_ERR_INVALID_ARGUMENT;

    char url[512];
    snprintf(url, sizeof(url), "%s/issues/%s", client->api_url, task_id);

#ifdef HU_IS_TEST
    (void)url;
    return HU_OK;
#endif

    char body[128];
    int blen = snprintf(body, sizeof(body), "{\"status\":\"%s\"}", status);

    char *auth = build_auth_header(client->alloc, client);
    char headers[256];
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\n%s%s",
             auth ? "Authorization: " : "", auth ? auth : "");
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(client->alloc, url, "PATCH", headers, body,
                                      (size_t)blen, &resp);
    if (auth)
        client->alloc->free(client->alloc->ctx, auth, strlen(auth) + 1);
    if (err != HU_OK)
        return err;

    hu_http_response_free(client->alloc, &resp);
    return HU_OK;
}

hu_error_t hu_paperclip_post_comment(hu_paperclip_client_t *client, const char *task_id,
                                      const char *body_text, size_t body_len) {
    if (!client || !task_id || !body_text)
        return HU_ERR_INVALID_ARGUMENT;

    char url[512];
    snprintf(url, sizeof(url), "%s/issues/%s/comments", client->api_url, task_id);

#ifdef HU_IS_TEST
    (void)url;
    (void)body_len;
    return HU_OK;
#endif

    size_t json_cap = body_len + 64;
    char *json_buf = (char *)client->alloc->alloc(client->alloc->ctx, json_cap);
    if (!json_buf)
        return HU_ERR_OUT_OF_MEMORY;

    int jlen = snprintf(json_buf, json_cap, "{\"body\":\"");
    for (size_t i = 0; i < body_len && (size_t)jlen < json_cap - 4; i++) {
        char c = body_text[i];
        if (c == '"' || c == '\\') {
            json_buf[jlen++] = '\\';
            json_buf[jlen++] = c;
        } else if (c == '\n') {
            json_buf[jlen++] = '\\';
            json_buf[jlen++] = 'n';
        } else if (c >= 0x20) {
            json_buf[jlen++] = c;
        }
    }
    jlen += snprintf(json_buf + jlen, json_cap - (size_t)jlen, "\"}");

    char *auth = build_auth_header(client->alloc, client);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(client->alloc, url, auth, json_buf, (size_t)jlen, &resp);
    if (auth)
        client->alloc->free(client->alloc->ctx, auth, strlen(auth) + 1);
    client->alloc->free(client->alloc->ctx, json_buf, json_cap);
    if (err != HU_OK)
        return err;

    hu_http_response_free(client->alloc, &resp);
    return HU_OK;
}

hu_error_t hu_paperclip_get_comments(hu_paperclip_client_t *client, const char *task_id,
                                      hu_paperclip_comment_list_t *out) {
    if (!client || !task_id || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url), "%s/issues/%s/comments", client->api_url, task_id);

#ifdef HU_IS_TEST
    (void)url;
    return HU_OK;
#endif

    char *auth = build_auth_header(client->alloc, client);
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(client->alloc, url, auth, &resp);
    if (auth)
        client->alloc->free(client->alloc->ctx, auth, strlen(auth) + 1);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(client->alloc, &resp);
        return HU_ERR_IO;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(client->alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(client->alloc, &resp);
    if (err != HU_OK)
        return err;

    hu_json_value_t *items = root;
    if (root->type == HU_JSON_OBJECT)
        items = hu_json_object_get(root, "items");
    if (!items)
        items = root;

    if (items->type == HU_JSON_ARRAY && items->data.array.len > 0) {
        size_t n = items->data.array.len;
        out->comments = (hu_paperclip_comment_t *)client->alloc->alloc(
            client->alloc->ctx, n * sizeof(hu_paperclip_comment_t));
        if (!out->comments) {
            hu_json_free(client->alloc, root);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(out->comments, 0, n * sizeof(hu_paperclip_comment_t));
        for (size_t i = 0; i < n; i++) {
            hu_json_value_t *c = items->data.array.items[i];
            if (!c || c->type != HU_JSON_OBJECT) continue;
            const char *s;
            s = hu_json_get_string(c, "id");
            if (s) out->comments[i].id = hu_strdup(client->alloc, s);
            s = hu_json_get_string(c, "body");
            if (s) out->comments[i].body = hu_strdup(client->alloc, s);
            s = hu_json_get_string(c, "authorName");
            if (!s) s = hu_json_get_string(c, "author_name");
            if (s) out->comments[i].author_name = hu_strdup(client->alloc, s);
            s = hu_json_get_string(c, "createdAt");
            if (!s) s = hu_json_get_string(c, "created_at");
            if (s) out->comments[i].created_at = hu_strdup(client->alloc, s);
        }
        out->count = n;
    }
    hu_json_free(client->alloc, root);
    return HU_OK;
}
