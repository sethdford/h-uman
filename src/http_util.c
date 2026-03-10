#include "human/http_util.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include <string.h>

#if defined(HU_HTTP_CURL)
hu_error_t hu_http_util_post(hu_allocator_t *alloc, const char *url, size_t url_len,
                             const char *body, size_t body_len, const char *const *headers,
                             size_t header_count, char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    (void)url_len;
    (void)headers;
    (void)header_count;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_request(alloc, url, "POST", NULL, body, body_len, &resp);
    if (err != HU_OK)
        return err;
    *out_body = resp.body;
    *out_len = resp.body_len;
    return HU_OK;
}

hu_error_t hu_http_util_get(hu_allocator_t *alloc, const char *url, size_t url_len,
                            const char *const *headers, size_t header_count,
                            const char *timeout_secs, char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    (void)headers;
    (void)header_count;
    (void)timeout_secs;
    (void)url_len;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK)
        return err;
    *out_body = resp.body;
    *out_len = resp.body_len;
    return HU_OK;
}
#else
hu_error_t hu_http_util_post(hu_allocator_t *alloc, const char *url, size_t url_len,
                             const char *body, size_t body_len, const char *const *headers,
                             size_t header_count, char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    (void)url_len;
    (void)body;
    (void)body_len;
    (void)headers;
    (void)header_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_http_util_get(hu_allocator_t *alloc, const char *url, size_t url_len,
                            const char *const *headers, size_t header_count,
                            const char *timeout_secs, char **out_body, size_t *out_len) {
    if (!alloc || !url || !out_body || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    (void)url_len;
    (void)headers;
    (void)header_count;
    (void)timeout_secs;
    return HU_ERR_NOT_SUPPORTED;
}
#endif
