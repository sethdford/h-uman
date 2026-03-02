#include "seaclaw/tools/web_search_providers.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SearXNG: self-hosted, uses base_url instead of api_key. No auth. */
sc_error_t sc_web_search_searxng(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *base_url,
    sc_tool_result_t *out)
{
    if (!alloc || !query || !base_url || !out) return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0 || count < 1 || count > 10) return SC_ERR_INVALID_ARGUMENT;

    /* Strip trailing slash from base_url if present */
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') base_len--;

    char *encoded = NULL;
    size_t enc_len = 0;
    sc_error_t err = sc_web_search_url_encode(alloc, query, query_len, &encoded, &enc_len);
    if (err != SC_OK) return err;

    /* /search?q=...&format=json&categories=general&pageno=1 */
    char url_buf[2048];
    int n = snprintf(url_buf, sizeof(url_buf),
        "%.*s/search?q=%.*s&format=json&categories=general&pageno=1",
        (int)base_len, base_url, (int)enc_len, encoded);
    alloc->free(alloc->ctx, encoded, enc_len + 1);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        *out = sc_tool_result_fail("URL too long", 13);
        return SC_OK;
    }

    sc_http_response_t resp = {0};
    err = sc_http_get_ex(alloc, url_buf, "Accept: application/json", &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("SearXNG search request failed", 31);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("SearXNG API error", 17);
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
    int nn = snprintf(buf, cap, "Results for: %.*s\n\n", (int)query_len, query);
    if (nn > 0) len = (size_t)nn;

    int max_r = count;
    if (max_r > (int)results->data.array.len) max_r = (int)results->data.array.len;
    for (int i = 0; i < max_r; i++) {
        sc_json_value_t *item = results->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) continue;
        const char *title = sc_json_get_string(item, "title");
        const char *url = sc_json_get_string(item, "url");
        const char *desc = sc_json_get_string(item, "content");
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
