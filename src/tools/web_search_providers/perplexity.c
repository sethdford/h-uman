#include "human/core/allocator.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERPLEXITY_API_URL "https://api.perplexity.ai/search"

hu_error_t hu_web_search_perplexity(hu_allocator_t *alloc, const char *query, size_t query_len,
                                    int count, const char *api_key, hu_tool_result_t *out) {
    if (!alloc || !query || !api_key || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10)
        return HU_ERR_INVALID_ARGUMENT;

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
    int bn = snprintf(body_buf, sizeof(body_buf), "{\"query\":\"%s\",\"max_results\":%d}", escaped,
                      count);
    if (bn <= 0 || (size_t)bn >= sizeof(body_buf)) {
        *out = hu_tool_result_fail("request body too long", 21);
        return HU_OK;
    }

    /* Perplexity uses Authorization: Bearer */
    char auth_buf[512];
    snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", api_key);

    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_post_json(alloc, PERPLEXITY_API_URL, auth_buf, body_buf, (size_t)bn, &resp);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("Perplexity search request failed", 34);
        return HU_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_fail("Perplexity API error", 21);
        return HU_OK;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed) {
        *out = hu_tool_result_fail("Failed to parse response", 24);
        return HU_OK;
    }

    hu_json_value_t *results = hu_json_object_get(parsed, "results");
    if (!results || results->type != HU_JSON_ARRAY || results->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        *out = hu_tool_result_ok_owned(hu_strndup(alloc, "No web results found.", 20), 20);
        return HU_OK;
    }

    hu_error_t fmt_err = hu_web_search_format_results(alloc, query, query_len, results, count,
                                                      "title", "url", "snippet", out);
    hu_json_free(alloc, parsed);
    return fmt_err;
}
