#include "human/core/allocator.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRAVE_API_URL "https://api.search.brave.com/res/v1/web/search"

hu_error_t hu_web_search_brave(hu_allocator_t *alloc, const char *query, size_t query_len,
                               int count, const char *api_key, hu_tool_result_t *out) {
    if (!alloc || !query || !api_key || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10)
        return HU_ERR_INVALID_ARGUMENT;

    char *encoded = NULL;
    size_t enc_len = 0;
    hu_error_t err = hu_web_search_url_encode(alloc, query, query_len, &encoded, &enc_len);
    if (err != HU_OK)
        return err;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%s?q=%.*s&count=%d", BRAVE_API_URL, (int)enc_len,
                     encoded, count);
    alloc->free(alloc->ctx, encoded, enc_len + 1); /* url_encode returns exact len+1 */
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        *out = hu_tool_result_fail("URL too long", 13);
        return HU_OK;
    }

    char headers_buf[512];
    int hn = snprintf(headers_buf, sizeof(headers_buf),
                      "X-Subscription-Token: %s\nAccept: application/json", api_key);
    if (hn < 0 || (size_t)hn >= sizeof(headers_buf)) {
        *out = hu_tool_result_fail("headers too long", 15);
        return HU_OK;
    }

    hu_http_response_t resp = {0};
    err = hu_http_get_ex(alloc, url_buf, headers_buf, &resp);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("Brave search request failed", 28);
        return HU_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_fail("Brave API error", 15);
        return HU_OK;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed) {
        *out = hu_tool_result_fail("Failed to parse Brave response", 31);
        return HU_OK;
    }

    hu_json_value_t *web = hu_json_object_get(parsed, "web");
    if (!web || web->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, parsed);
        *out = hu_tool_result_ok_owned(hu_strndup(alloc, "No web results found.", 20), 20);
        return HU_OK;
    }
    hu_json_value_t *results = hu_json_object_get(web, "results");
    if (!results || results->type != HU_JSON_ARRAY || results->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        *out = hu_tool_result_ok_owned(hu_strndup(alloc, "No web results found.", 20), 20);
        return HU_OK;
    }

    hu_error_t fmt_err = hu_web_search_format_results(alloc, query, query_len, results, count,
                                                      "title", "url", "description", out);
    hu_json_free(alloc, parsed);
    return fmt_err;
}
