#include "seaclaw/http_util.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/error.h"
#include <string.h>

#if defined(SC_HTTP_CURL)
sc_error_t sc_http_util_post(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *body, size_t body_len,
    const char *const *headers, size_t header_count,
    char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len) return SC_ERR_INVALID_ARGUMENT;
    (void)url_len;
    (void)headers;
    (void)header_count;
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_request(alloc,
        url, "POST", NULL,
        body, body_len, &resp);
    if (err != SC_OK) return err;
    *out_body = resp.body;
    *out_len = resp.body_len;
    return SC_OK;
}

sc_error_t sc_http_util_get(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *const *headers, size_t header_count,
    const char *timeout_secs,
    char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len) return SC_ERR_INVALID_ARGUMENT;
    (void)headers;
    (void)header_count;
    (void)timeout_secs;
    (void)url_len;
    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(alloc, url, NULL, &resp);
    if (err != SC_OK) return err;
    *out_body = resp.body;
    *out_len = resp.body_len;
    return SC_OK;
}
#else
sc_error_t sc_http_util_post(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *body, size_t body_len,
    const char *const *headers, size_t header_count,
    char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len) return SC_ERR_INVALID_ARGUMENT;
    (void)url_len;
    (void)body;
    (void)body_len;
    (void)headers;
    (void)header_count;
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_http_util_get(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *const *headers, size_t header_count,
    const char *timeout_secs,
    char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len) return SC_ERR_INVALID_ARGUMENT;
    (void)url_len;
    (void)headers;
    (void)header_count;
    (void)timeout_secs;
    return SC_ERR_NOT_SUPPORTED;
}
#endif
