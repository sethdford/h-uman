#include "seaclaw/tools/web_search_providers.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DDG_API_URL "https://api.duckduckgo.com/"

sc_error_t sc_web_search_duckduckgo(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count,
    sc_tool_result_t *out)
{
    if (!alloc || !query || !out) return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10) return SC_ERR_INVALID_ARGUMENT;

    char *encoded = NULL;
    size_t enc_len = 0;
    sc_error_t err = sc_web_search_url_encode(alloc, query, query_len, &encoded, &enc_len);
    if (err != SC_OK) return err;

    char url_buf[1024];
    int n = snprintf(url_buf, sizeof(url_buf), "%s?q=%.*s&format=json&no_html=1&skip_disambig=1",
        DDG_API_URL, (int)enc_len, encoded);
    alloc->free(alloc->ctx, encoded, enc_len + 1);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        *out = sc_tool_result_fail("URL too long", 13);
        return SC_OK;
    }

    sc_http_response_t resp = {0};
    err = sc_http_get_ex(alloc, url_buf, "Accept: application/json", &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("DuckDuckGo request failed", 27);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("DuckDuckGo API error", 21);
        return SC_OK;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &parsed);
    sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !parsed) {
        *out = sc_tool_result_fail("Failed to parse DuckDuckGo response", 36);
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
    int wrote = snprintf(buf, cap, "Results for: %.*s\n\n", (int)query_len, query);
    if (wrote > 0) len = (size_t)wrote;

    int idx = 1;
    const char *heading = sc_json_get_string(parsed, "Heading");
    const char *abstract = sc_json_get_string(parsed, "AbstractText");
    const char *abstract_url = sc_json_get_string(parsed, "AbstractURL");
    if (abstract_url && abstract_url[0] && abstract && abstract[0] && idx <= count) {
        const char *t = heading && heading[0] ? heading : abstract;
        wrote = snprintf(buf + len, cap - len, "%d. %s\n   %s\n   %s\n\n", idx, t, abstract_url, abstract);
        if (wrote > 0) len += (size_t)wrote;
        idx++;
    }

    sc_json_value_t *related = sc_json_object_get(parsed, "RelatedTopics");
    if (related && related->type == SC_JSON_ARRAY) {
        for (size_t i = 0; i < related->data.array.len && idx <= count; i++) {
            sc_json_value_t *topic = related->data.array.items[i];
            if (!topic || topic->type != SC_JSON_OBJECT) continue;
            const char *text = sc_json_get_string(topic, "Text");
            const char *first_url = sc_json_get_string(topic, "FirstURL");
            if (text && text[0] && first_url && first_url[0]) {
                wrote = snprintf(buf + len, cap - len, "%d. %s\n   %s\n   %s\n\n", idx, text, first_url, text);
                if (wrote > 0) len += (size_t)wrote;
                idx++;
            }
        }
    }

    sc_json_free(alloc, parsed);
    if (idx == 1) {
        alloc->free(alloc->ctx, buf, cap);
        *out = sc_tool_result_ok_owned(sc_strndup(alloc, "No web results found.", 20), 20);
        return SC_OK;
    }
    *out = sc_tool_result_ok_owned(buf, len);
    return SC_OK;
}
