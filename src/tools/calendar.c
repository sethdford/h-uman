#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/calendar_tool.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TOOL_NAME "calendar"
#define TOOL_DESC                                                                        \
    "Manage Google Calendar events. Actions: list (upcoming events), create (new event " \
    "with title, start, end, optional description), delete (remove an event by event_id)."
#define TOOL_PARAMS                                                                               \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\","    \
    "\"create\",\"delete\"]},\"calendar_id\":{\"type\":\"string\","                               \
    "\"description\":\"Calendar ID (default: primary)\"},\"event_id\":{\"type\":\"string\"},"     \
    "\"title\":{\"type\":\"string\"},\"start\":{\"type\":\"string\",\"description\":"             \
    "\"ISO 8601 datetime\"},\"end\":{\"type\":\"string\",\"description\":\"ISO 8601 datetime\"}," \
    "\"description\":{\"type\":\"string\"},\"max_results\":"                                      \
    "{\"type\":\"number\",\"description\":\"Max events to return (default: 10)\"},"               \
    "\"access_token\":{\"type\":\"string\",\"description\":\"OAuth2 access token\"}},"            \
    "\"required\":[\"action\"]}"

#define GCAL_API "https://www.googleapis.com/calendar/v3/calendars/"

typedef struct {
    char _unused;
} calendar_ctx_t;

bool hu_calendar_id_is_safe(const char *id, size_t max_len) {
    if (!id || !id[0])
        return false;
    size_t n = 0;
    for (const char *p = id; *p; p++, n++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '@' || c == '-';
        if (!ok || n >= max_len)
            return false;
    }
    return true;
}

hu_error_t hu_calendar_event_body(hu_allocator_t *alloc, const char *title, const char *start,
                                  const char *end, const char *desc, char **out, size_t *out_len) {
    if (!alloc || !title || !start || !end || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    hu_json_value_t *root = hu_json_object_new(alloc);
    hu_json_value_t *s = hu_json_object_new(alloc);
    hu_json_value_t *e = hu_json_object_new(alloc);
    if (!root || !s || !e) {
        if (root)
            hu_json_free(alloc, root);
        if (s)
            hu_json_free(alloc, s);
        if (e)
            hu_json_free(alloc, e);
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_json_object_set(alloc, s, "dateTime", hu_json_string_new(alloc, start, strlen(start)));
    hu_json_object_set(alloc, e, "dateTime", hu_json_string_new(alloc, end, strlen(end)));
    hu_json_object_set(alloc, root, "summary", hu_json_string_new(alloc, title, strlen(title)));
    hu_json_object_set(alloc, root, "start", s);
    hu_json_object_set(alloc, root, "end", e);
    if (desc)
        hu_json_object_set(alloc, root, "description",
                           hu_json_string_new(alloc, desc, strlen(desc)));
    hu_error_t err = hu_json_stringify(alloc, root, out, out_len);
    hu_json_free(alloc, root);
    return err;
}

bool hu_calendar_status_ok(long status_code) {
    return status_code >= 200 && status_code < 300;
}

static hu_error_t calendar_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
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
            "{\"events\":[{\"id\":\"evt1\",\"title\":\"Team Standup\",\"start\":"
            "\"2026-03-03T09:00:00Z\",\"end\":\"2026-03-03T09:30:00Z\"},{\"id\":\"evt2\","
            "\"title\":\"1:1 with CTO\",\"start\":\"2026-03-03T14:00:00Z\",\"end\":"
            "\"2026-03-03T14:30:00Z\"}]}";
        *out = hu_tool_result_ok(resp, strlen(resp));
    } else if (strcmp(action, "create") == 0) {
        const char *title = hu_json_get_string(args, "title");
        char *msg = hu_sprintf(alloc, "{\"created\":true,\"id\":\"evt_new\",\"title\":\"%s\"}",
                               title ? title : "Untitled");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "delete") == 0) {
        *out = hu_tool_result_ok("{\"deleted\":true}", 16);
    } else {
        /* Same answer as production: only list/create/delete exist. */
        *out = hu_tool_result_fail("unsupported action", 18);
    }
    return HU_OK;
#else
    const char *token = hu_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        *out = hu_tool_result_fail("missing access_token — configure Google Calendar OAuth2 token",
                                   62);
        return HU_OK;
    }
    const char *cal_id = hu_json_get_string(args, "calendar_id");
    if (!cal_id)
        cal_id = "primary";
    if (!hu_calendar_id_is_safe(cal_id, 200)) {
        *out = hu_tool_result_fail("invalid calendar_id", 19);
        return HU_OK;
    }

    if (strcmp(action, "list") == 0) {
        int max_results = (int)hu_json_get_number(args, "max_results", 10);
        char url[512];
        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        char time_min[32];
        strftime(time_min, sizeof(time_min), "%Y-%m-%dT%H:%M:%SZ", tm);
        snprintf(url, sizeof(url),
                 "%s%s/events?maxResults=%d&timeMin=%s&orderBy=startTime"
                 "&singleEvents=true",
                 GCAL_API, cal_id, max_results, time_min);

        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url, auth, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to list events", 21);
            return HU_OK;
        }
        char *body = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "create") == 0) {
        const char *title = hu_json_get_string(args, "title");
        const char *start = hu_json_get_string(args, "start");
        const char *end = hu_json_get_string(args, "end");
        const char *desc = hu_json_get_string(args, "description");
        if (!title || !start || !end) {
            *out = hu_tool_result_fail("create needs title, start, end", 30);
            return HU_OK;
        }
        char *body = NULL;
        size_t body_len = 0;
        if (hu_calendar_event_body(alloc, title, start, end, desc, &body, &body_len) != HU_OK) {
            *out = hu_tool_result_fail("failed to build event", 21);
            return HU_OK;
        }
        char url[256];
        snprintf(url, sizeof(url), "%s%s/events", GCAL_API, cal_id);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, url, auth, body, body_len, &resp);
        alloc->free(alloc->ctx, body, body_len + 1);
        if (err != HU_OK || !hu_calendar_status_ok(resp.status_code)) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to create event", 22);
            return HU_OK;
        }
        char *rbody = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "delete") == 0) {
        const char *event_id = hu_json_get_string(args, "event_id");
        if (!event_id || strlen(event_id) == 0) {
            *out = hu_tool_result_fail("missing event_id", 16);
            return HU_OK;
        }
        if (!hu_calendar_id_is_safe(event_id, 1024)) {
            *out = hu_tool_result_fail("invalid event_id", 16);
            return HU_OK;
        }
        char url[1400];
        snprintf(url, sizeof(url), "%s%s/events/%s", GCAL_API, cal_id, event_id);
        char headers[300];
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\n", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_request(alloc, url, "DELETE", headers, NULL, 0, &resp);
        long status = resp.status_code;
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !hu_calendar_status_ok(status)) {
            *out = (status == 404 || status == 410)
                       ? hu_tool_result_fail("event not found", 15)
                       : hu_tool_result_fail("failed to delete event", 22);
            return HU_OK;
        }
        *out = hu_tool_result_ok("{\"deleted\":true}", 16);
    } else {
        *out = hu_tool_result_fail("unsupported action", 18);
    }
    return HU_OK;
#endif
}

static const char *calendar_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *calendar_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *calendar_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void calendar_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(calendar_ctx_t));
}

static const hu_tool_vtable_t calendar_vtable = {
    .execute = calendar_execute,
    .name = calendar_name,
    .description = calendar_desc,
    .parameters_json = calendar_params,
    .deinit = calendar_deinit,
};

hu_error_t hu_calendar_create(hu_allocator_t *alloc, hu_tool_t *out) {
    void *ctx = alloc->alloc(alloc->ctx, sizeof(calendar_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(calendar_ctx_t));
    out->ctx = ctx;
    out->vtable = &calendar_vtable;
    return HU_OK;
}
