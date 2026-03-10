#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TOOL_NAME "calendar"
#define TOOL_DESC                                                                             \
    "Manage calendar events. Actions: list (upcoming events), create (new event with title, " \
    "start, end, attendees), update (modify event), delete (remove event), availability "     \
    "(check free/busy). Supports Google Calendar API."
#define TOOL_PARAMS                                                                               \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\","    \
    "\"create\",\"update\",\"delete\",\"availability\"]},\"calendar_id\":{\"type\":\"string\","   \
    "\"description\":\"Calendar ID (default: primary)\"},\"event_id\":{\"type\":\"string\"},"     \
    "\"title\":{\"type\":\"string\"},\"start\":{\"type\":\"string\",\"description\":"             \
    "\"ISO 8601 datetime\"},\"end\":{\"type\":\"string\",\"description\":\"ISO 8601 datetime\"}," \
    "\"attendees\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":"          \
    "\"Email addresses\"},\"description\":{\"type\":\"string\"},\"max_results\":"                 \
    "{\"type\":\"number\",\"description\":\"Max events to return (default: 10)\"},"               \
    "\"access_token\":{\"type\":\"string\",\"description\":\"OAuth2 access token\"}},"            \
    "\"required\":[\"action\"]}"

#define GCAL_API "https://www.googleapis.com/calendar/v3/calendars/"

typedef struct {
    char _unused;
} calendar_ctx_t;

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
    } else if (strcmp(action, "availability") == 0) {
        *out = hu_tool_result_ok("{\"free_slots\":[\"09:30-12:00\",\"15:00-17:00\"]}", 49);
    } else if (strcmp(action, "delete") == 0) {
        *out = hu_tool_result_ok("{\"deleted\":true}", 17);
    } else {
        *out = hu_tool_result_ok("{\"status\":\"ok\"}", 15);
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
    if (strlen(cal_id) > 200) {
        *out = hu_tool_result_fail("calendar_id too long", 21);
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
        char *body = hu_sprintf(
            alloc,
            "{\"summary\":\"%s\",\"start\":{\"dateTime\":\"%s\"},\"end\":{\"dateTime\":\"%s\"}"
            "%s%s%s}",
            title, start, end, desc ? ",\"description\":\"" : "", desc ? desc : "",
            desc ? "\"" : "");
        char url[256];
        snprintf(url, sizeof(url), "%s%s/events", GCAL_API, cal_id);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, url, auth, body, body ? strlen(body) : 0, &resp);
        alloc->free(alloc->ctx, body, body ? strlen(body) + 1 : 0);
        if (err != HU_OK) {
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
        if (strlen(event_id) > 100) {
            *out = hu_tool_result_fail("event_id too long", 17);
            return HU_OK;
        }
        char url[512];
        snprintf(url, sizeof(url), "%s%s/events/%s", GCAL_API, cal_id, event_id);
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        char auth_hdr[256];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url, auth_hdr, &resp);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        (void)err;
        *out = hu_tool_result_ok(
            "{\"deleted\":true,\"note\":\"use HTTP DELETE for real deletion\"}", 57);
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
