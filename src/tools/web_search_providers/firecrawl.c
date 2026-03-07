#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRECRAWL_API_URL "https://api.firecrawl.dev/v1/search"

sc_error_t sc_web_search_firecrawl(sc_allocator_t *alloc, const char *query, size_t query_len,
                                   int count, const char *api_key, sc_tool_result_t *out) {
    if (!alloc || !query || !api_key || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10)
        return SC_ERR_INVALID_ARGUMENT;

    /* Escape query for JSON */
    char escaped[1024];
    size_t ej = 0;
    for (size_t i = 0; i < query_len && ej + 2 < sizeof(escaped); i++) {
        char c = query[i];
        if (c == '"' || c == '\\') {
            escaped[ej++] = '\\';
            escaped[ej++] = c;
        } else if (c == '\n') {
            escaped[ej++] = '\\';
            escaped[ej++] = 'n';
        } else if (c == '\r') {
            escaped[ej++] = '\\';
            escaped[ej++] = 'r';
        } else if ((unsigned char)c >= 32)
            escaped[ej++] = c;
    }
    escaped[ej] = '\0';

    char body_buf[1024];
    int bn = snprintf(body_buf, sizeof(body_buf),
                      "{\"query\":\"%s\",\"limit\":%d,\"timeout\":10000}", escaped, count);
    if (bn <= 0 || (size_t)bn >= sizeof(body_buf)) {
        *out = sc_tool_result_fail("request body too long", 21);
        return SC_OK;
    }

    /* Firecrawl uses Authorization: Bearer */
    char auth_buf[512];
    snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", api_key);

    sc_http_response_t resp = {0};
    sc_error_t err =
        sc_http_post_json(alloc, FIRECRAWL_API_URL, auth_buf, body_buf, (size_t)bn, &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Firecrawl search request failed", 32);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Firecrawl API error", 19);
        return SC_OK;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed) {
        *out = sc_tool_result_fail("Failed to parse response", 24);
        return SC_OK;
    }

    /* Firecrawl returns data[] not results[] */
    sc_json_value_t *results = sc_json_object_get(parsed, "data");
    if (!results || results->type != SC_JSON_ARRAY || results->data.array.len == 0) {
        sc_json_free(alloc, parsed);
        *out = sc_tool_result_ok_owned(sc_strndup(alloc, "No web results found.", 20), 20);
        return SC_OK;
    }

    sc_error_t fmt_err = sc_web_search_format_results(alloc, query, query_len, results, count,
                                                      "title", "url", "description", out);
    sc_json_free(alloc, parsed);
    return fmt_err;
}
