#include "human/providers/provider_http.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hu_error_t hu_provider_http_post_json(hu_allocator_t *alloc, const char *url,
                                      const char *auth_header, const char *extra_headers,
                                      const char *body, size_t body_len,
                                      hu_json_value_t **parsed_out) {
    return hu_provider_http_post_json_opts(alloc, url, auth_header, extra_headers, body, body_len,
                                           NULL, parsed_out);
}

hu_error_t hu_provider_http_post_json_opts(hu_allocator_t *alloc, const char *url,
                                           const char *auth_header, const char *extra_headers,
                                           const char *body, size_t body_len,
                                           const hu_http_request_opts_t *opts,
                                           hu_json_value_t **parsed_out) {
    if (!alloc || !url || !parsed_out)
        return HU_ERR_INVALID_ARGUMENT;

    *parsed_out = NULL;

    hu_http_response_t hresp = {0};
    hu_error_t err;

    if (extra_headers && extra_headers[0]) {
        err = hu_http_post_json_opts(alloc, url, auth_header, extra_headers, body, body_len, opts,
                                     &hresp);
    } else {
        err = hu_http_post_json_opts(alloc, url, auth_header, NULL, body, body_len, opts, &hresp);
    }

    if (err != HU_OK)
        return err;

    if (hresp.status_code < 200 || hresp.status_code >= 300) {
        hu_log_error("provider_http", NULL, "HTTP %ld (body_len=%zu)", hresp.status_code,
                     hresp.body_len);
        hu_error_t status_err =
            hu_provider_http_status_error(hresp.status_code, hresp.body, hresp.body_len);
        hu_http_response_free(alloc, &hresp);
        return status_err;
    }

    err = hu_json_parse(alloc, hresp.body, hresp.body_len, parsed_out);
    hu_http_response_free(alloc, &hresp);
    return err;
}

static bool body_contains(const char *body, size_t len, const char *needle) {
    size_t nl = strlen(needle);
    if (!body || len < nl)
        return false;
    for (size_t i = 0; i + nl <= len; i++)
        if (memcmp(body + i, needle, nl) == 0)
            return true;
    return false;
}

hu_error_t hu_provider_http_status_error(long status, const char *body, size_t body_len) {
    if (status == 401)
        return HU_ERR_PROVIDER_AUTH;
    if (status == 429)
        return HU_ERR_PROVIDER_RATE_LIMITED;
    if (status == 415 || (status == 422 && body_contains(body, body_len, "unsupported_modality")))
        return HU_ERR_NOT_SUPPORTED;
    return HU_ERR_PROVIDER_RESPONSE;
}
