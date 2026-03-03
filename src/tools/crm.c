#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "crm"
#define TOOL_DESC                                                                             \
    "Manage CRM contacts and deals (HubSpot API). Actions: contacts (list/search contacts), " \
    "contact_create (new contact), deals (list deals), deal_create (new deal), deal_update "  \
    "(change deal stage/amount), notes (add note to contact/deal)."
#define TOOL_PARAMS                                                                               \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":["             \
    "\"contacts\",\"contact_create\",\"deals\",\"deal_create\",\"deal_update\",\"notes\"]},"      \
    "\"api_key\":{\"type\":\"string\",\"description\":\"HubSpot API key\"},\"email\":"            \
    "{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"company\":{\"type\":\"string\"},"     \
    "\"phone\":{\"type\":\"string\"},\"deal_name\":{\"type\":\"string\"},\"amount\":"             \
    "{\"type\":\"number\"},\"stage\":{\"type\":\"string\"},\"contact_id\":{\"type\":\"string\"}," \
    "\"deal_id\":{\"type\":\"string\"},\"note\":{\"type\":\"string\"},\"query\":"                 \
    "{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"action\"]}"

#define HUBSPOT_API "https://api.hubapi.com"

typedef struct {
    char _unused;
} crm_ctx_t;

static sc_error_t crm_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
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
    if (strcmp(action, "contacts") == 0) {
        *out = sc_tool_result_ok(
            "{\"contacts\":[{\"id\":\"101\",\"name\":\"Alice Smith\",\"email\":\"alice@acme.com\","
            "\"company\":\"Acme Corp\"},{\"id\":\"102\",\"name\":\"Bob Jones\","
            "\"email\":\"bob@widgets.io\",\"company\":\"Widgets Inc\"}]}",
            199);
    } else if (strcmp(action, "contact_create") == 0) {
        const char *name = sc_json_get_string(args, "name");
        char *msg = sc_sprintf(alloc, "{\"created\":true,\"id\":\"103\",\"name\":\"%s\"}",
                               name ? name : "Unknown");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "deals") == 0) {
        *out = sc_tool_result_ok(
            "{\"deals\":[{\"id\":\"d1\",\"name\":\"Enterprise License\",\"amount\":50000,"
            "\"stage\":\"negotiation\"},{\"id\":\"d2\",\"name\":\"Starter Plan\","
            "\"amount\":5000,\"stage\":\"closed_won\"}]}",
            182);
    } else if (strcmp(action, "deal_create") == 0) {
        const char *deal_name = sc_json_get_string(args, "deal_name");
        char *msg = sc_sprintf(alloc, "{\"created\":true,\"id\":\"d3\",\"name\":\"%s\"}",
                               deal_name ? deal_name : "New Deal");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "deal_update") == 0) {
        *out = sc_tool_result_ok("{\"updated\":true}", 17);
    } else if (strcmp(action, "notes") == 0) {
        *out = sc_tool_result_ok("{\"added\":true}", 15);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
#else
    const char *api_key = sc_json_get_string(args, "api_key");
    if (!api_key || strlen(api_key) == 0) {
        *out = sc_tool_result_fail("missing api_key for HubSpot", 27);
        return SC_OK;
    }
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);

    if (strcmp(action, "contacts") == 0) {
        sc_http_response_t resp = {0};
        sc_error_t err =
            sc_http_get(alloc, HUBSPOT_API "/crm/v3/objects/contacts?limit=20", auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to list contacts", 23);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "deals") == 0) {
        sc_http_response_t resp = {0};
        sc_error_t err =
            sc_http_get(alloc, HUBSPOT_API "/crm/v3/objects/deals?limit=20", auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to list deals", 20);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(action, "contact_create") == 0) {
        const char *email_val = sc_json_get_string(args, "email");
        const char *name_val = sc_json_get_string(args, "name");
        const char *company = sc_json_get_string(args, "company");
        const char *phone = sc_json_get_string(args, "phone");
        char *body = sc_sprintf(alloc,
            "{\"properties\":{%s%s%s%s%s%s%s%s%s%s%s%s%s%s}}",
            email_val ? "\"email\":\"" : "", email_val ? email_val : "", email_val ? "\"" : "",
            (email_val && name_val) ? "," : "",
            name_val ? "\"firstname\":\"" : "", name_val ? name_val : "", name_val ? "\"" : "",
            ((name_val || email_val) && company) ? "," : "",
            company ? "\"company\":\"" : "", company ? company : "", company ? "\"" : "",
            phone ? ",\"phone\":\"" : "", phone ? phone : "", phone ? "\"" : "");
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc,
            HUBSPOT_API "/crm/v3/objects/contacts", auth,
            body, body ? strlen(body) : 0, &resp);
        if (body) alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to create contact", 24);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "deal_create") == 0) {
        const char *deal_name = sc_json_get_string(args, "deal_name");
        double amount = sc_json_get_number(args, "amount", 0);
        const char *stage = sc_json_get_string(args, "stage");
        char *body = sc_sprintf(alloc,
            "{\"properties\":{\"dealname\":\"%s\",\"amount\":\"%.2f\"%s%s%s}}",
            deal_name ? deal_name : "New Deal", amount,
            stage ? ",\"dealstage\":\"" : "", stage ? stage : "", stage ? "\"" : "");
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc,
            HUBSPOT_API "/crm/v3/objects/deals", auth,
            body, body ? strlen(body) : 0, &resp);
        if (body) alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to create deal", 21);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(action, "deal_update") == 0) {
        const char *deal_id = sc_json_get_string(args, "deal_id");
        if (!deal_id) { *out = sc_tool_result_fail("missing deal_id", 15); return SC_OK; }
        const char *stage = sc_json_get_string(args, "stage");
        double amount = sc_json_get_number(args, "amount", -1);
        char *body = sc_sprintf(alloc, "{\"properties\":{%s%s%s%s}}",
            stage ? "\"dealstage\":\"" : "", stage ? stage : "", stage ? "\"" : "",
            amount >= 0 ? ",\"amount\":\"" : "");
        char url[256];
        snprintf(url, sizeof(url), HUBSPOT_API "/crm/v3/objects/deals/%s", deal_id);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, url, auth,
            body, body ? strlen(body) : 0, &resp);
        if (body) alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to update deal", 21);
            return SC_OK;
        }
        if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok("{\"updated\":true}", 17);
    } else if (strcmp(action, "notes") == 0) {
        const char *note = sc_json_get_string(args, "note");
        const char *contact_id = sc_json_get_string(args, "contact_id");
        if (!note) { *out = sc_tool_result_fail("missing note", 12); return SC_OK; }
        char *body = sc_sprintf(alloc,
            "{\"properties\":{\"hs_note_body\":\"%s\"}%s%s%s}",
            note,
            contact_id ? ",\"associations\":[{\"to\":{\"id\":\"" : "",
            contact_id ? contact_id : "",
            contact_id ? "\"},\"types\":[{\"associationCategory\":\"HUBSPOT_DEFINED\","
                         "\"associationTypeId\":202}]}]" : "");
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc,
            HUBSPOT_API "/crm/v3/objects/notes", auth,
            body, body ? strlen(body) : 0, &resp);
        if (body) alloc->free(alloc->ctx, body, strlen(body) + 1);
        if (err != SC_OK) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to add note", 18);
            return SC_OK;
        }
        char *rbody = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
#endif
}

static const char *crm_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *crm_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *crm_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void crm_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t crm_vtable = {
    .execute = crm_execute,
    .name = crm_name,
    .description = crm_desc,
    .parameters_json = crm_params,
    .deinit = crm_deinit,
};

sc_error_t sc_crm_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(crm_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &crm_vtable;
    return SC_OK;
}
