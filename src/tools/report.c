#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOOL_NAME "report"
#define TOOL_DESC                                                                                 \
    "Generate structured reports in Markdown or HTML. Actions: create (build report with title, " \
    "sections, and optional data tables), template (list available report templates), export "    \
    "(convert report to specified format)."
#define TOOL_PARAMS                                                                               \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\","  \
    "\"template\",\"export\"]},\"title\":{\"type\":\"string\"},\"sections\":{\"type\":\"array\"," \
    "\"items\":{\"type\":\"object\",\"properties\":{\"heading\":{\"type\":\"string\"},"           \
    "\"content\":{\"type\":\"string\"}}}},\"format\":{\"type\":\"string\",\"enum\":["             \
    "\"markdown\",\"html\"],\"description\":\"Output format (default: markdown)\"},"              \
    "\"template\":{\"type\":\"string\",\"description\":\"Template name (executive_summary, "      \
    "weekly_status, incident_report, financial_summary)\"}},\"required\":[\"action\"]}"

typedef struct {
    char _unused;
} report_ctx_t;

static sc_error_t report_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
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

    if (strcmp(action, "template") == 0) {
        const char *list =
            "Available templates:\n"
            "- executive_summary: Title, Key Metrics, Highlights, Risks, Next Steps\n"
            "- weekly_status: Title, Accomplishments, In Progress, Blockers, Plan\n"
            "- incident_report: Title, Summary, Timeline, Impact, Root Cause, Remediation\n"
            "- financial_summary: Title, Revenue, Expenses, Net, Forecast\n";
        *out = sc_tool_result_ok(list, strlen(list));
        return SC_OK;
    }

    if (strcmp(action, "create") == 0) {
        const char *title = sc_json_get_string(args, "title");
        const char *format = sc_json_get_string(args, "format");
        bool html = format && strcmp(format, "html") == 0;

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char date[32];
        strftime(date, sizeof(date), "%Y-%m-%d", tm);

        size_t buf_sz = 16384;
        char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 13);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = 0;
        if (html) {
            n += snprintf(msg + n, buf_sz - (size_t)n,
                          "<!DOCTYPE html><html><head><title>%s</title>"
                          "<style>body{font-family:system-ui;max-width:800px;margin:40px auto;"
                          "padding:0 20px}h1{border-bottom:2px solid #333}h2{color:#555}"
                          "table{border-collapse:collapse;width:100%%}td,th{border:1px solid "
                          "#ddd;padding:8px;text-align:left}</style></head><body>"
                          "<h1>%s</h1><p><em>Generated: %s</em></p>",
                          title ? title : "Report", title ? title : "Report", date);
        } else {
            n += snprintf(msg + n, buf_sz - (size_t)n, "# %s\n\n*Generated: %s*\n\n",
                          title ? title : "Report", date);
        }

        sc_json_value_t *sections = sc_json_object_get((sc_json_value_t *)args, "sections");
        if (sections && sections->type == SC_JSON_ARRAY) {
            for (size_t i = 0; i < sections->data.array.len; i++) {
                sc_json_value_t *sec = sections->data.array.items[i];
                if (!sec || sec->type != SC_JSON_OBJECT)
                    continue;
                const char *heading = sc_json_get_string(sec, "heading");
                const char *content = sc_json_get_string(sec, "content");
                if (html) {
                    n += snprintf(msg + n, buf_sz - (size_t)n, "<h2>%s</h2><p>%s</p>",
                                  heading ? heading : "", content ? content : "");
                } else {
                    n += snprintf(msg + n, buf_sz - (size_t)n, "## %s\n\n%s\n\n",
                                  heading ? heading : "", content ? content : "");
                }
            }
        }

        if (html)
            n += snprintf(msg + n, buf_sz - (size_t)n, "</body></html>");

        *out = sc_tool_result_ok_owned(msg, (size_t)n);
        return SC_OK;
    }

    if (strcmp(action, "export") == 0) {
        *out = sc_tool_result_ok("Use 'create' with format='html' or format='markdown'", 51);
        return SC_OK;
    }

    *out = sc_tool_result_fail("unknown action", 14);
    return SC_OK;
}

static const char *report_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *report_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *report_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void report_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t report_vtable = {
    .execute = report_execute,
    .name = report_name,
    .description = report_desc,
    .parameters_json = report_params,
    .deinit = report_deinit,
};

sc_error_t sc_report_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(report_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &report_vtable;
    return SC_OK;
}
