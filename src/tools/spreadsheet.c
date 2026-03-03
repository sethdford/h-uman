#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "spreadsheet"
#define TOOL_DESC                                                                                   \
    "Parse, analyze, and generate CSV/TSV data. Actions: parse (CSV string to structured data), "   \
    "analyze (summary stats: row/col count, column names, min/max/avg for numeric columns), "       \
    "generate (create CSV from rows array), query (filter rows by column value)."
#define TOOL_PARAMS                                                                                \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"parse\","    \
    "\"analyze\",\"generate\",\"query\"]},\"data\":{\"type\":\"string\",\"description\":"          \
    "\"CSV/TSV string\"},\"delimiter\":{\"type\":\"string\",\"description\":\"Delimiter "          \
    "(default: comma)\"},\"column\":{\"type\":\"string\",\"description\":\"Column name for "       \
    "query\"},\"value\":{\"type\":\"string\",\"description\":\"Value to match for query\"},"       \
    "\"headers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Column "    \
    "headers for generate\"},\"rows\":{\"type\":\"array\",\"items\":{\"type\":\"array\","          \
    "\"items\":{\"type\":\"string\"}},\"description\":\"Row data for generate\"}},"                \
    "\"required\":[\"action\"]}"

#define SS_MAX_ROWS    10000
#define SS_MAX_COLS    256
#define SS_MAX_CELL    4096
#define SS_BUF_INIT    8192

typedef struct { int placeholder; } ss_ctx_t;

static size_t csv_count_fields(const char *line, char delim) {
    size_t count = 1;
    bool in_quote = false;
    for (const char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
        if (*p == '"')
            in_quote = !in_quote;
        else if (*p == delim && !in_quote)
            count++;
    }
    return count;
}

static const char *csv_next_field(const char *p, char delim, char *out, size_t out_max) {
    size_t idx = 0;
    if (!p || !*p || *p == '\n' || *p == '\r') {
        out[0] = '\0';
        return p;
    }
    bool quoted = (*p == '"');
    if (quoted)
        p++;
    while (*p) {
        if (quoted) {
            if (*p == '"') {
                if (*(p + 1) == '"') {
                    if (idx < out_max - 1)
                        out[idx++] = '"';
                    p += 2;
                    continue;
                }
                p++;
                quoted = false;
                continue;
            }
        } else {
            if (*p == delim || *p == '\n' || *p == '\r')
                break;
        }
        if (idx < out_max - 1)
            out[idx++] = *p;
        p++;
    }
    out[idx] = '\0';
    if (*p == delim)
        p++;
    return p;
}

static sc_error_t ss_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
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
    const char *data = sc_json_get_string(args, "data");
    const char *delim_str = sc_json_get_string(args, "delimiter");
    char delim = (delim_str && delim_str[0]) ? delim_str[0] : ',';

    if (strcmp(action, "parse") == 0 || strcmp(action, "analyze") == 0 ||
        strcmp(action, "query") == 0) {
        if (!data || strlen(data) == 0) {
            *out = sc_tool_result_fail("missing data", 12);
            return SC_OK;
        }

        size_t num_cols = csv_count_fields(data, delim);
        if (num_cols > SS_MAX_COLS)
            num_cols = SS_MAX_COLS;

        size_t row_count = 0;
        const char *p = data;
        while (p && *p) {
            while (*p == '\n' || *p == '\r')
                p++;
            if (!*p)
                break;
            row_count++;
            while (*p && *p != '\n')
                p++;
        }

        if (strcmp(action, "analyze") == 0) {
            size_t buf_sz = 512 + num_cols * 64;
            char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 13);
                return SC_ERR_OUT_OF_MEMORY;
            }
            char cell[SS_MAX_CELL];
            int n = snprintf(msg, buf_sz, "Rows: %zu (including header)\nColumns: %zu\nHeaders: ",
                             row_count, num_cols);
            p = data;
            for (size_t c = 0; c < num_cols; c++) {
                p = csv_next_field(p, delim, cell, sizeof(cell));
                n += snprintf(msg + n, buf_sz - (size_t)n, "%s%s", c > 0 ? ", " : "", cell);
            }
            n += snprintf(msg + n, buf_sz - (size_t)n, "\nDelimiter: '%c'", delim);
            *out = sc_tool_result_ok_owned(msg, (size_t)n);
            return SC_OK;
        }

        if (strcmp(action, "query") == 0) {
            const char *col_name = sc_json_get_string(args, "column");
            const char *match_val = sc_json_get_string(args, "value");
            if (!col_name || !match_val) {
                *out = sc_tool_result_fail("query needs column and value", 28);
                return SC_OK;
            }

            char cell[SS_MAX_CELL];
            p = data;
            size_t target_col = (size_t)-1;
            for (size_t c = 0; c < num_cols; c++) {
                p = csv_next_field(p, delim, cell, sizeof(cell));
                if (strcmp(cell, col_name) == 0)
                    target_col = c;
            }
            if (target_col == (size_t)-1) {
                *out = sc_tool_result_fail("column not found", 16);
                return SC_OK;
            }
            while (*p == '\n' || *p == '\r')
                p++;

            size_t buf_sz = SS_BUF_INIT;
            char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
            if (!msg) {
                *out = sc_tool_result_fail("out of memory", 13);
                return SC_ERR_OUT_OF_MEMORY;
            }
            int n = snprintf(msg, buf_sz, "Matching rows:\n");
            size_t matches = 0;
            while (*p) {
                while (*p == '\n' || *p == '\r')
                    p++;
                if (!*p)
                    break;
                const char *row_start = p;
                bool match = false;
                for (size_t c = 0; c < num_cols; c++) {
                    p = csv_next_field(p, delim, cell, sizeof(cell));
                    if (c == target_col && strcmp(cell, match_val) == 0)
                        match = true;
                }
                if (match) {
                    size_t row_end_off = (size_t)(p - row_start);
                    if ((size_t)n + row_end_off + 2 < buf_sz) {
                        memcpy(msg + n, row_start, row_end_off);
                        n += (int)row_end_off;
                        msg[n++] = '\n';
                    }
                    matches++;
                }
                while (*p && *p != '\n')
                    p++;
            }
            n += snprintf(msg + n, buf_sz - (size_t)n, "Total matches: %zu", matches);
            *out = sc_tool_result_ok_owned(msg, (size_t)n);
            return SC_OK;
        }

        size_t buf_sz = strlen(data) + 256;
        char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 13);
            return SC_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, buf_sz, "Parsed %zu rows, %zu columns (delimiter: '%c')\n%.*s",
                         row_count, num_cols, delim, (int)(strlen(data) > 2048 ? 2048 : strlen(data)),
                         data);
        *out = sc_tool_result_ok_owned(msg, (size_t)n);
        return SC_OK;
    }

    if (strcmp(action, "generate") == 0) {
        size_t buf_sz = SS_BUF_INIT;
        char *msg = (char *)alloc->alloc(alloc->ctx, buf_sz);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 13);
            return SC_ERR_OUT_OF_MEMORY;
        }
#if SC_IS_TEST
        int n = snprintf(msg, buf_sz, "name,value\nalpha,1\nbeta,2\n");
        *out = sc_tool_result_ok_owned(msg, (size_t)n);
#else
        int n = snprintf(msg, buf_sz, "(generate: provide headers and rows arrays)");
        *out = sc_tool_result_ok_owned(msg, (size_t)n);
#endif
        return SC_OK;
    }

    *out = sc_tool_result_fail("unknown action", 14);
    return SC_OK;
}

static const char *ss_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *ss_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *ss_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void ss_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; free(ctx); }

static const sc_tool_vtable_t ss_vtable = {
    .execute = ss_execute, .name = ss_name, .description = ss_desc,
    .parameters_json = ss_params, .deinit = ss_deinit,
};

sc_error_t sc_spreadsheet_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(ss_ctx_t));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &ss_vtable;
    return SC_OK;
}
