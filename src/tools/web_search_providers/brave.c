#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRAVE_API_URL "https://api.search.brave.com/res/v1/web/search"

sc_error_t sc_web_search_brave(sc_allocator_t *alloc, const char *query, size_t query_len,
                               int count, const char *api_key, sc_tool_result_t *out) {
    if (!alloc || !query || !api_key || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10)
        return SC_ERR_INVALID_ARGUMENT;

    char *encoded = NULL;
    size_t enc_len = 0;
    sc_error_t err = sc_web_search_url_encode(alloc, query, query_len, &encoded, &enc_len);
    if (err != SC_OK)
        return err;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%s?q=%.*s&count=%d", BRAVE_API_URL, (int)enc_len,
                     encoded, count);
    alloc->free(alloc->ctx, encoded, enc_len + 1); /* url_encode returns exact len+1 */
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        *out = sc_tool_result_fail("URL too long", 13);
        return SC_OK;
    }

    char headers_buf[512];
    int hn = snprintf(headers_buf, sizeof(headers_buf),
                      "X-Subscription-Token: %s\nAccept: application/json", api_key);
    if (hn < 0 || (size_t)hn >= sizeof(headers_buf)) {
        *out = sc_tool_result_fail("headers too long", 15);
        return SC_OK;
    }

    sc_http_response_t resp = {0};
    err = sc_http_get_ex(alloc, url_buf, headers_buf, &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Brave search request failed", 28);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Brave API error", 15);
        return SC_OK;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed) {
        *out = sc_tool_result_fail("Failed to parse Brave response", 31);
        return SC_OK;
    }

    sc_json_value_t *web = sc_json_object_get(parsed, "web");
    if (!web || web->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, parsed);
        *out = sc_tool_result_ok_owned(sc_strndup(alloc, "No web results found.", 20), 20);
        return SC_OK;
    }
    sc_json_value_t *results = sc_json_object_get(web, "results");
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
