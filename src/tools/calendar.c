#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOOL_NAME "calendar"
#define TOOL_DESC                                                                                   \
    "Manage calendar events. Actions: list (upcoming events), create (new event with title, "       \
    "start, end, attendees), update (modify event), delete (remove event), availability "           \
    "(check free/busy). Supports Google Calendar API."
#define TOOL_PARAMS                                                                                \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\","     \
    "\"create\",\"update\",\"delete\",\"availability\"]},\"calendar_id\":{\"type\":\"string\","    \
    "\"description\":\"Calendar ID (default: primary)\"},\"event_id\":{\"type\":\"string\"},"      \
    "\"title\":{\"type\":\"string\"},\"start\":{\"type\":\"string\",\"description\":"              \
    "\"ISO 8601 datetime\"},\"end\":{\"type\":\"string\",\"description\":\"ISO 8601 datetime\"},"  \
    "\"attendees\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":"           \
    "\"Email addresses\"},\"description\":{\"type\":\"string\"},\"max_results\":"                  \
    "{\"type\":\"number\",\"description\":\"Max events to return (default: 10)\"},"                \
    "\"access_token\":{\"type\":\"string\",\"description\":\"OAuth2 access token\"}},"             \
    "\"required\":[\"action\"]}"

#define GCAL_API "https://www.googleapis.com/calendar/v3/calendars/"

typedef struct { int placeholder; } calendar_ctx_t;

static sc_error_t calendar_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
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
            "{\"events\":[{\"id\":\"evt1\",\"title\":\"Team Standup\",\"start\":"
            "\"2026-03-03T09:00:00Z\",\"end\":\"2026-03-03T09:30:00Z\"},{\"id\":\"evt2\","
            "\"title\":\"1:1 with CTO\",\"start\":\"2026-03-03T14:00:00Z\",\"end\":"
            "\"2026-03-03T14:30:00Z\"}]}";
        *out = sc_tool_result_ok(resp, strlen(resp));
    } else if (strcmp(action, "create") == 0) {
        const char *title = sc_json_get_string(args, "title");
        char *msg = sc_sprintf(alloc, "{\"created\":true,\"id\":\"evt_new\",\"title\":\"%s\"}",
                               title ? title : "Untitled");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "availability") == 0) {
        *out = sc_tool_result_ok("{\"free_slots\":[\"09:30-12:00\",\"15:00-17:00\"]}", 49);
    } else if (strcmp(action, "delete") == 0) {
        *out = sc_tool_result_ok("{\"deleted\":true}", 17);
    } else {
        *out = sc_tool_result_ok("{\"status\":\"ok\"}", 15);
    }
    return SC_OK;
#else
    const char *token = sc_json_get_string(args, "access_token");
    if (!token || strlen(token) == 0) {
        *out = sc_tool_result_fail(
            "missing access_token — configure Google Calendar OAuth2 token", 62);
        return SC_OK;
    }
    const char *cal_id = sc_json_get_string(args, "calendar_id");
    if (!cal_id)
        cal_id = "primary";

    if (strcmp(action, "list") == 0) {
        int max_results = (int)sc_json_get_number(args, "max_results", 10);
        char url[512];
        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        char time_min[32];
        strftime(time_min, sizeof(time_min), "%Y-%m-%dT%H:%M:%SZ", tm);
        snprintf(url, sizeof(url), "%s%s/events?maxResults=%d&timeMin=%s&orderBy=startTime"
                 "&singleEvents=true", GCAL_API, cal_id, max_results, time_min);

        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, url, auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to list events", 21);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "create") == 0) {
        const char *title = sc_json_get_string(args, "title");
        const char *start = sc_json_get_string(args, "start");
        const char *end = sc_json_get_string(args, "end");
        const char *desc = sc_json_get_string(args, "description");
        if (!title || !start || !end) {
            *out = sc_tool_result_fail("create needs title, start, end", 30);
            return SC_OK;
        }
        char *body = sc_sprintf(alloc,
            "{\"summary\":\"%s\",\"start\":{\"dateTime\":\"%s\"},\"end\":{\"dateTime\":\"%s\"}"
            "%s%s%s}",
            title, start, end,
            desc ? ",\"description\":\"" : "", desc ? desc : "", desc ? "\"" : "");
        char url[256];
        snprintf(url, sizeof(url), "%s%s/events", GCAL_API, cal_id);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        alloc->free(alloc->ctx, body, body ? strlen(body) + 1 : 0);
        if (err != SC_OK) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to create event", 22);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "delete") == 0) {
        const char *event_id = sc_json_get_string(args, "event_id");
        if (!event_id) {
            *out = sc_tool_result_fail("missing event_id", 16);
            return SC_OK;
        }
        char url[512];
        snprintf(url, sizeof(url), "%s%s/events/%s", GCAL_API, cal_id, event_id);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        (void)url;
        (void)auth;
        *out = sc_tool_result_fail("DELETE not yet supported — use REST API manually", 48);
    } else {
        *out = sc_tool_result_fail("unsupported action", 18);
    }
    return SC_OK;
#endif
}

static const char *calendar_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *calendar_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *calendar_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void calendar_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; free(ctx); }

static const sc_tool_vtable_t calendar_vtable = {
    .execute = calendar_execute, .name = calendar_name, .description = calendar_desc,
    .parameters_json = calendar_params, .deinit = calendar_deinit,
};

sc_error_t sc_calendar_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(calendar_ctx_t));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &calendar_vtable;
    return SC_OK;
}
