#include "human/core/allocator.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDG_API_URL "https://api.duckduckgo.com/"

hu_error_t hu_web_search_duckduckgo(hu_allocator_t *alloc, const char *query, size_t query_len,
                                    int count, hu_tool_result_t *out) {
    if (!alloc || !query || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10)
        return HU_ERR_INVALID_ARGUMENT;

    char *encoded = NULL;
    size_t enc_len = 0;
    hu_error_t err = hu_web_search_url_encode(alloc, query, query_len, &encoded, &enc_len);
    if (err != HU_OK)
        return err;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%s?q=%.*s&format=json&no_html=1&skip_disambig=1",
                     DDG_API_URL, (int)enc_len, encoded);
    alloc->free(alloc->ctx, encoded, enc_len + 1);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        *out = hu_tool_result_fail("URL too long", 13);
        return HU_OK;
    }

    hu_http_response_t resp = {0};
    err = hu_http_get_ex(alloc, url_buf, "Accept: application/json", &resp);
    if (err != HU_OK) {
        *out = hu_tool_result_fail("DuckDuckGo request failed", 27);
        return HU_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_fail("DuckDuckGo API error", 21);
        return HU_OK;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !parsed) {
        *out = hu_tool_result_fail("Failed to parse DuckDuckGo response", 36);
        return HU_OK;
    }

    size_t cap = 4096;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        hu_json_free(alloc, parsed);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    int wrote = snprintf(buf, cap, "Results for: %.*s\n\n", (int)query_len, query);
    if (wrote > 0)
        len = (size_t)wrote;

    int idx = 1;
    const char *heading = hu_json_get_string(parsed, "Heading");
    const char *abstract = hu_json_get_string(parsed, "AbstractText");
    const char *abstract_url = hu_json_get_string(parsed, "AbstractURL");
    if (abstract_url && abstract_url[0] && abstract && abstract[0] && idx <= count) {
        const char *t = heading && heading[0] ? heading : abstract;
        wrote = snprintf(buf + len, cap - len, "%d. %s\n   %s\n   %s\n\n", idx, t, abstract_url,
                         abstract);
        if (wrote > 0)
            len += (size_t)wrote;
        idx++;
    }

    hu_json_value_t *related = hu_json_object_get(parsed, "RelatedTopics");
    if (related && related->type == HU_JSON_ARRAY) {
        for (size_t i = 0; i < related->data.array.len && idx <= count; i++) {
            hu_json_value_t *topic = related->data.array.items[i];
            if (!topic || topic->type != HU_JSON_OBJECT)
                continue;
            const char *text = hu_json_get_string(topic, "Text");
            const char *first_url = hu_json_get_string(topic, "FirstURL");
            if (text && text[0] && first_url && first_url[0]) {
                wrote = snprintf(buf + len, cap - len, "%d. %s\n   %s\n   %s\n\n", idx, text,
                                 first_url, text);
                if (wrote > 0)
                    len += (size_t)wrote;
                idx++;
            }
        }
    }

    hu_json_free(alloc, parsed);
    if (idx == 1) {
        alloc->free(alloc->ctx, buf, cap);
        *out = hu_tool_result_ok_owned(hu_strndup(alloc, "No web results found.", 20), 20);
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(buf, len);
    return HU_OK;
}
