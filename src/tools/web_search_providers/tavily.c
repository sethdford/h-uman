#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAVILY_API_URL "https://api.tavily.com/search"

sc_error_t sc_web_search_tavily(sc_allocator_t *alloc, const char *query, size_t query_len,
                                int count, const char *api_key, sc_tool_result_t *out) {
    if (!alloc || !query || !api_key || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10)
        return SC_ERR_INVALID_ARGUMENT;

    char body_buf[2048];
    /* Escape query and api_key for safe JSON interpolation */
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

    char key_escaped[512];
    size_t kj = 0;
    size_t key_len = strlen(api_key);
    for (size_t i = 0; i < key_len && kj + 2 < sizeof(key_escaped); i++) {
        char c = api_key[i];
        if (c == '"' || c == '\\') {
            key_escaped[kj++] = '\\';
            key_escaped[kj++] = c;
        } else if ((unsigned char)c >= 32)
            key_escaped[kj++] = c;
    }
    key_escaped[kj] = '\0';

    int n = snprintf(
        body_buf, sizeof(body_buf),
        "{\"api_key\":\"%s\",\"query\":\"%s\",\"max_results\":%d,\"search_depth\":\"basic\","
        "\"include_answer\":false,\"include_raw_content\":false,\"include_images\":false}",
        key_escaped, escaped, count);
    if (n <= 0 || (size_t)n >= sizeof(body_buf)) {
        *out = sc_tool_result_fail("Request body too long", 21);
        return SC_OK;
    }

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json(alloc, TAVILY_API_URL, NULL, body_buf, (size_t)n, &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Tavily request failed", 22);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Tavily API error", 16);
        return SC_OK;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed) {
        *out = sc_tool_result_fail("Failed to parse Tavily response", 32);
        return SC_OK;
    }

    if (sc_json_object_get(parsed, "error")) {
        sc_json_free(alloc, parsed);
        *out = sc_tool_result_fail("Tavily API returned error", 26);
        return SC_OK;
    }

    sc_json_value_t *results = sc_json_object_get(parsed, "results");
    if (!results || results->type != SC_JSON_ARRAY || results->data.array.len == 0) {
        sc_json_free(alloc, parsed);
        *out = sc_tool_result_ok_owned(sc_strndup(alloc, "No web results found.", 20), 20);
        return SC_OK;
    }

    sc_error_t fmt_err = sc_web_search_format_results(alloc, query, query_len, results, count,
                                                      "title", "url", "content", out);
    sc_json_free(alloc, parsed);
    return fmt_err;
}
