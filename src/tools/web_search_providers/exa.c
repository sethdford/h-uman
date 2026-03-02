#include "seaclaw/tools/web_search_providers.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXA_API_URL "https://api.exa.ai/search"

sc_error_t sc_web_search_exa(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out)
{
    if (!alloc || !query || !api_key || !out) return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10) return SC_ERR_INVALID_ARGUMENT;

    /* Escape query for JSON */
    char escaped[1024];
    size_t ej = 0;
    for (size_t i = 0; i < query_len && ej + 2 < sizeof(escaped); i++) {
        char c = query[i];
        if (c == '"' || c == '\\') { escaped[ej++] = '\\'; escaped[ej++] = c; }
        else if (c == '\n') { escaped[ej++] = '\\'; escaped[ej++] = 'n'; }
        else if (c == '\r') { escaped[ej++] = '\\'; escaped[ej++] = 'r'; }
        else if ((unsigned char)c >= 32) escaped[ej++] = c;
    }
    escaped[ej] = '\0';

    char body_buf[1024];
    int bn = snprintf(body_buf, sizeof(body_buf),
        "{\"query\":\"%s\",\"numResults\":%d}", escaped, count);
    if (bn <= 0 || (size_t)bn >= sizeof(body_buf)) {
        *out = sc_tool_result_fail("request body too long", 21);
        return SC_OK;
    }

    /* Exa uses x-api-key header, not Authorization (Content-Type added by impl) */
    char headers_buf[512];
    snprintf(headers_buf, sizeof(headers_buf),
        "x-api-key: %s\nAccept: application/json", api_key);

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json_ex(alloc, EXA_API_URL, NULL, headers_buf,
        body_buf, (size_t)bn, &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Exa search request failed", 26);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Exa API error", 14);
        return SC_OK;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed) {
        *out = sc_tool_result_fail("Failed to parse response", 24);
        return SC_OK;
    }

    sc_json_value_t *results = sc_json_object_get(parsed, "results");
    if (!results || results->type != SC_JSON_ARRAY || results->data.array.len == 0) {
        sc_json_free(alloc, parsed);
        *out = sc_tool_result_ok_owned(sc_strndup(alloc, "No web results found.", 20), 20);
        return SC_OK;
    }

    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        sc_json_free(alloc, parsed);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    int n = snprintf(buf, cap, "Results for: %.*s\n\n", (int)query_len, query);
    if (n > 0) len = (size_t)n;

    int max_r = count;
    if (max_r > (int)results->data.array.len) max_r = (int)results->data.array.len;
    for (int i = 0; i < max_r; i++) {
        sc_json_value_t *item = results->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) continue;
        const char *title = sc_json_get_string(item, "title");
        const char *url = sc_json_get_string(item, "url");
        const char *desc = sc_json_get_string(item, "text");
        if (!title) title = ""; if (!url) url = ""; if (!desc) desc = "";

        char line[1024];
        int ln = snprintf(line, sizeof(line), "%d. %s\n   %s\n   %s\n\n", i + 1, title, url, desc);
        if (ln > 0 && len + (size_t)ln < cap) {
            memcpy(buf + len, line, (size_t)ln + 1);
            len += (size_t)ln;
        } else if (ln > 0) {
            size_t new_cap = cap * 2;
            char *nbuf = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nbuf) break;
            buf = nbuf; cap = new_cap;
            memcpy(buf + len, line, (size_t)ln + 1);
            len += (size_t)ln;
        }
    }
    sc_json_free(alloc, parsed);
    *out = sc_tool_result_ok_owned(buf, len);
    return SC_OK;
}
