#include "seaclaw/tools/web_search_providers.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JINA_BASE_URL "https://s.jina.ai/"

/* Jina Reader: GET to https://s.jina.ai/<url_encoded_query>, returns plain text.
 * count parameter is ignored; Jina returns a single reader-style result. */
sc_error_t sc_web_search_jina(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    int count, const char *api_key,
    sc_tool_result_t *out)
{
    (void)count;
    if (!alloc || !query || !api_key || !out) return SC_ERR_INVALID_ARGUMENT;
    if (query_len == 0) return SC_ERR_INVALID_ARGUMENT;

    char *encoded = NULL;
    size_t enc_len = 0;
    sc_error_t err = sc_web_search_url_encode(alloc, query, query_len, &encoded, &enc_len);
    if (err != SC_OK) return err;

    char url_buf[2048];
    int n = snprintf(url_buf, sizeof(url_buf), "%s%.*s",
        JINA_BASE_URL, (int)enc_len, encoded);
    alloc->free(alloc->ctx, encoded, enc_len + 1);
    if (n <= 0 || (size_t)n >= sizeof(url_buf)) {
        *out = sc_tool_result_fail("URL too long", 13);
        return SC_OK;
    }

    /* Jina needs both Authorization: Bearer and x-api-key, plus Accept: text/plain */
    char headers_buf[768];
    snprintf(headers_buf, sizeof(headers_buf),
        "Authorization: Bearer %s\nx-api-key: %s\nAccept: text/plain", api_key, api_key);

    sc_http_response_t resp = {0};
    err = sc_http_get_ex(alloc, url_buf, headers_buf, &resp);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Jina request failed", 19);
        return SC_OK;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("Jina API error", 15);
        return SC_OK;
    }

    /* Jina returns plain text body directly. Wrap in "Results for: <query>\n\n<body>" */
    size_t prefix_len = 14 + query_len + 2; /* "Results for: " + query + "\n\n" */
    size_t total_len = prefix_len + resp.body_len;
    if (total_len > 1024 * 1024) total_len = 1024 * 1024; /* cap at 1MB */

    char *buf = (char *)alloc->alloc(alloc->ctx, total_len + 1);
    if (!buf) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }

    int pn = snprintf(buf, total_len + 1, "Results for: %.*s\n\n", (int)query_len, query);
    size_t len = (pn > 0) ? (size_t)pn : 0;
    size_t body_to_copy = (total_len > len) ? (total_len - len) : 0;
    if (body_to_copy > 0 && resp.body && resp.body_len > 0) {
        if (body_to_copy > resp.body_len) body_to_copy = resp.body_len;
        memcpy(buf + len, resp.body, body_to_copy);
        len += body_to_copy;
    }
    buf[len] = '\0';

    sc_http_response_free(alloc, &resp);
    *out = sc_tool_result_ok_owned(buf, len);
    return SC_OK;
}
