#include "human/a2a.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <string.h>
#include <stdio.h>

#ifndef HU_IS_TEST
#define HU_IS_TEST 0
#endif

#define A2A_URL_BUF_SIZE 2048

static char *a2a_dup(hu_allocator_t *alloc, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = alloc->alloc(alloc->ctx, len + 1);
    if (d) { memcpy(d, s, len); d[len] = 0; }
    return d;
}

#ifdef HU_HTTP_CURL
static hu_a2a_task_state_t a2a_parse_state(const char *s) {
    if (!s) return HU_A2A_TASK_SUBMITTED;
    if (strcmp(s, "submitted") == 0) return HU_A2A_TASK_SUBMITTED;
    if (strcmp(s, "working") == 0) return HU_A2A_TASK_WORKING;
    if (strcmp(s, "input_required") == 0) return HU_A2A_TASK_INPUT_REQUIRED;
    if (strcmp(s, "completed") == 0) return HU_A2A_TASK_COMPLETED;
    if (strcmp(s, "failed") == 0) return HU_A2A_TASK_FAILED;
    if (strcmp(s, "canceled") == 0) return HU_A2A_TASK_CANCELED;
    return HU_A2A_TASK_SUBMITTED;
}
#endif /* HU_HTTP_CURL */

hu_error_t hu_a2a_discover(hu_allocator_t *alloc, const char *agent_url, hu_a2a_agent_card_t *out) {
    if (!alloc || !agent_url || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (HU_IS_TEST) {
        out->name = a2a_dup(alloc, "test-agent");
        out->description = a2a_dup(alloc, "A test agent");
        out->url = a2a_dup(alloc, agent_url);
        out->version = a2a_dup(alloc, "1.0.0");
        return HU_OK;
    }
#if defined(HU_HTTP_CURL)
    {
        char url_buf[A2A_URL_BUF_SIZE];
        size_t len = strlen(agent_url);
        int n = snprintf(url_buf, sizeof(url_buf), "%s%s.well-known/agent.json",
                         agent_url, (len > 0 && agent_url[len - 1] == '/') ? "" : "/");
        if (n < 0 || (size_t)n >= sizeof(url_buf))
            return HU_ERR_INVALID_ARGUMENT;

        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url_buf, NULL, &resp);
        if (err != HU_OK)
            return err;

        if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            return HU_ERR_IO;
        }

        hu_json_value_t *root = NULL;
        err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
            if (root) hu_json_free(alloc, root);
            return err != HU_OK ? err : HU_ERR_JSON_PARSE;
        }

        const char *name = hu_json_get_string(root, "name");
        const char *desc = hu_json_get_string(root, "description");
        const char *url = hu_json_get_string(root, "url");
        const char *version = hu_json_get_string(root, "version");

        out->name = a2a_dup(alloc, name ? name : "");
        out->description = a2a_dup(alloc, desc ? desc : "");
        out->url = a2a_dup(alloc, url ? url : agent_url);
        out->version = a2a_dup(alloc, version ? version : "");

        hu_json_value_t *skills_arr = hu_json_object_get(root, "skills");
        if (skills_arr && skills_arr->type == HU_JSON_ARRAY && skills_arr->data.array.len > 0) {
            size_t cnt = skills_arr->data.array.len;
            hu_a2a_skill_t *skills = (hu_a2a_skill_t *)alloc->alloc(alloc->ctx, cnt * sizeof(hu_a2a_skill_t));
            if (skills) {
                memset(skills, 0, cnt * sizeof(hu_a2a_skill_t));
                out->skills = skills;
                out->skills_count = cnt;
                for (size_t i = 0; i < cnt; i++) {
                    hu_json_value_t *item = skills_arr->data.array.items[i];
                    if (item && item->type == HU_JSON_OBJECT) {
                        const char *id = hu_json_get_string(item, "id");
                        const char *sname = hu_json_get_string(item, "name");
                        const char *sdesc = hu_json_get_string(item, "description");
                        skills[i].id = a2a_dup(alloc, id);
                        skills[i].name = a2a_dup(alloc, sname);
                        skills[i].description = a2a_dup(alloc, sdesc);
                    }
                }
            }
        }

        hu_json_free(alloc, root);
        return HU_OK;
    }
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_a2a_send_task(hu_allocator_t *alloc, const char *agent_url, const hu_a2a_message_t *message, hu_a2a_task_t *out) {
    if (!alloc || !agent_url || !message || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (HU_IS_TEST) {
        out->id = a2a_dup(alloc, "task-001");
        out->state = HU_A2A_TASK_SUBMITTED;
        return HU_OK;
    }
#if defined(HU_HTTP_CURL)
    {
        hu_json_buf_t buf;
        hu_error_t err = hu_json_buf_init(&buf, alloc);
        if (err != HU_OK) return err;

        err = hu_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tasks/send\",\"params\":{\"message\":{", 68);
        if (err != HU_OK) goto send_cleanup;

        err = hu_json_append_key_value(&buf, "role", 4, message->role ? message->role : "user",
                                       message->role ? strlen(message->role) : 4);
        if (err != HU_OK) goto send_cleanup;

        err = hu_json_buf_append_raw(&buf, ",\"parts\":[", 10);
        if (err != HU_OK) goto send_cleanup;

        for (size_t i = 0; i < message->parts_count; i++) {
            if (i > 0) { err = hu_json_buf_append_raw(&buf, ",", 1); if (err != HU_OK) goto send_cleanup; }
            err = hu_json_buf_append_raw(&buf, "{\"type\":\"", 8);
            if (err != HU_OK) goto send_cleanup;
            const hu_a2a_part_t *p = &message->parts[i];
            if (p->type) {
                err = hu_json_append_string(&buf, p->type, strlen(p->type));
                if (err != HU_OK) goto send_cleanup;
            } else {
                err = hu_json_buf_append_raw(&buf, "\"text\"", 6);
                if (err != HU_OK) goto send_cleanup;
            }
            err = hu_json_buf_append_raw(&buf, ",\"content\":\"", 12);
            if (err != HU_OK) goto send_cleanup;
            if (p->content && p->content_len > 0) {
                err = hu_json_append_string(&buf, p->content, p->content_len);
                if (err != HU_OK) goto send_cleanup;
            }
            err = hu_json_buf_append_raw(&buf, "\"}", 2);
            if (err != HU_OK) goto send_cleanup;
        }

        err = hu_json_buf_append_raw(&buf, "]}}}", 4);
        if (err != HU_OK) goto send_cleanup;

        hu_http_response_t hresp = {0};
        err = hu_http_post_json(alloc, agent_url, NULL, buf.ptr, buf.len, &hresp);
        hu_json_buf_free(&buf);
        if (err != HU_OK) return err;

        if (hresp.status_code != 200 || !hresp.body || hresp.body_len == 0) {
            if (hresp.owned && hresp.body)
                hu_http_response_free(alloc, &hresp);
            return HU_ERR_IO;
        }

        hu_json_value_t *root = NULL;
        err = hu_json_parse(alloc, hresp.body, hresp.body_len, &root);
        if (hresp.owned && hresp.body)
            hu_http_response_free(alloc, &hresp);
        if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
            if (root) hu_json_free(alloc, root);
            return err != HU_OK ? err : HU_ERR_JSON_PARSE;
        }

        hu_json_value_t *result = hu_json_object_get(root, "result");
        if (result && result->type == HU_JSON_OBJECT) {
            const char *id = hu_json_get_string(result, "id");
            const char *state_str = hu_json_get_string(result, "state");
            out->id = a2a_dup(alloc, id);
            out->state = a2a_parse_state(state_str);
        }
        hu_json_free(alloc, root);
        return HU_OK;

send_cleanup:
        hu_json_buf_free(&buf);
        return err;
    }
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_a2a_get_task(hu_allocator_t *alloc, const char *agent_url, const char *task_id, hu_a2a_task_t *out) {
    if (!alloc || !agent_url || !task_id || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (HU_IS_TEST) {
        out->id = a2a_dup(alloc, task_id);
        out->state = HU_A2A_TASK_COMPLETED;
        return HU_OK;
    }
#if defined(HU_HTTP_CURL)
    {
        hu_json_buf_t buf;
        hu_error_t err = hu_json_buf_init(&buf, alloc);
        if (err != HU_OK) return err;

        err = hu_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tasks/get\",\"params\":{", 45);
        if (err != HU_OK) goto get_cleanup;
        err = hu_json_append_key_value(&buf, "id", 2, task_id, strlen(task_id));
        if (err != HU_OK) goto get_cleanup;
        err = hu_json_buf_append_raw(&buf, "}}", 2);
        if (err != HU_OK) goto get_cleanup;

        hu_http_response_t hresp = {0};
        err = hu_http_post_json(alloc, agent_url, NULL, buf.ptr, buf.len, &hresp);
        hu_json_buf_free(&buf);
        if (err != HU_OK) return err;

        if (hresp.status_code != 200 || !hresp.body || hresp.body_len == 0) {
            if (hresp.owned && hresp.body)
                hu_http_response_free(alloc, &hresp);
            return HU_ERR_IO;
        }

        hu_json_value_t *root = NULL;
        err = hu_json_parse(alloc, hresp.body, hresp.body_len, &root);
        if (hresp.owned && hresp.body)
            hu_http_response_free(alloc, &hresp);
        if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
            if (root) hu_json_free(alloc, root);
            return err != HU_OK ? err : HU_ERR_JSON_PARSE;
        }

        hu_json_value_t *result = hu_json_object_get(root, "result");
        if (result && result->type == HU_JSON_OBJECT) {
            const char *id = hu_json_get_string(result, "id");
            const char *state_str = hu_json_get_string(result, "state");
            out->id = a2a_dup(alloc, id ? id : task_id);
            out->state = a2a_parse_state(state_str);
        } else {
            out->id = a2a_dup(alloc, task_id);
        }
        hu_json_free(alloc, root);
        return HU_OK;

get_cleanup:
        hu_json_buf_free(&buf);
        return err;
    }
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_a2a_cancel_task(hu_allocator_t *alloc, const char *agent_url, const char *task_id) {
    if (!alloc || !agent_url || !task_id) return HU_ERR_INVALID_ARGUMENT;
    if (HU_IS_TEST) return HU_OK;
#if defined(HU_HTTP_CURL)
    {
        hu_json_buf_t buf;
        hu_error_t err = hu_json_buf_init(&buf, alloc);
        if (err != HU_OK) return err;

        err = hu_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tasks/cancel\",\"params\":{", 52);
        if (err != HU_OK) goto cancel_cleanup;
        err = hu_json_append_key_value(&buf, "id", 2, task_id, strlen(task_id));
        if (err != HU_OK) goto cancel_cleanup;
        err = hu_json_buf_append_raw(&buf, "}}", 2);
        if (err != HU_OK) goto cancel_cleanup;

        hu_http_response_t hresp = {0};
        err = hu_http_post_json(alloc, agent_url, NULL, buf.ptr, buf.len, &hresp);
        hu_json_buf_free(&buf);
        if (err != HU_OK) return err;

        if (hresp.status_code != 200) {
            if (hresp.owned && hresp.body)
                hu_http_response_free(alloc, &hresp);
            return HU_ERR_IO;
        }
        if (hresp.owned && hresp.body)
            hu_http_response_free(alloc, &hresp);
        return HU_OK;

cancel_cleanup:
        hu_json_buf_free(&buf);
        return err;
    }
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_a2a_server_init(hu_allocator_t *alloc, const hu_a2a_agent_card_t *card) {
    if (!alloc || !card) return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_a2a_server_handle_request(hu_allocator_t *alloc, const char *method, const char *body, size_t body_len, char **response, size_t *response_len) {
    if (!alloc || !method || !response || !response_len) return HU_ERR_INVALID_ARGUMENT;
    const char *resp;
    if (strcmp(method, "GET") == 0) {
        resp = "{\"name\":\"human\",\"version\":\"1.0\"}";
    } else if (strcmp(method, "POST") == 0 && body && body_len > 0) {
        resp = "{\"jsonrpc\":\"2.0\",\"result\":{\"id\":\"task-001\",\"state\":\"completed\"}}";
    } else {
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t len = strlen(resp);
    *response = alloc->alloc(alloc->ctx, len + 1);
    if (!*response) return HU_ERR_OUT_OF_MEMORY;
    memcpy(*response, resp, len + 1);
    *response_len = len;
    return HU_OK;
}

void hu_a2a_server_deinit(hu_allocator_t *alloc) { (void)alloc; }

void hu_a2a_agent_card_free(hu_allocator_t *alloc, hu_a2a_agent_card_t *card) {
    if (!alloc || !card) return;
    if (card->name) { alloc->free(alloc->ctx, card->name, strlen(card->name)+1); card->name = NULL; }
    if (card->description) { alloc->free(alloc->ctx, card->description, strlen(card->description)+1); card->description = NULL; }
    if (card->url) { alloc->free(alloc->ctx, card->url, strlen(card->url)+1); card->url = NULL; }
    if (card->version) { alloc->free(alloc->ctx, card->version, strlen(card->version)+1); card->version = NULL; }
    if (card->skills) {
        for (size_t i = 0; i < card->skills_count; i++) {
            if (card->skills[i].id) { alloc->free(alloc->ctx, card->skills[i].id, strlen(card->skills[i].id)+1); card->skills[i].id = NULL; }
            if (card->skills[i].name) { alloc->free(alloc->ctx, card->skills[i].name, strlen(card->skills[i].name)+1); card->skills[i].name = NULL; }
            if (card->skills[i].description) { alloc->free(alloc->ctx, card->skills[i].description, strlen(card->skills[i].description)+1); card->skills[i].description = NULL; }
        }
        alloc->free(alloc->ctx, card->skills, card->skills_count * sizeof(hu_a2a_skill_t));
        card->skills = NULL;
        card->skills_count = 0;
    }
}

void hu_a2a_task_free(hu_allocator_t *alloc, hu_a2a_task_t *task) {
    if (!alloc || !task) return;
    if (task->id) { alloc->free(alloc->ctx, task->id, strlen(task->id)+1); task->id = NULL; }
}

void hu_a2a_message_free(hu_allocator_t *alloc, hu_a2a_message_t *msg) { (void)alloc; (void)msg; }
