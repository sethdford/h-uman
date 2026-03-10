#include "human/providers/provider_http.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hu_error_t hu_provider_http_post_json(hu_allocator_t *alloc, const char *url,
                                      const char *auth_header, const char *extra_headers,
                                      const char *body, size_t body_len,
                                      hu_json_value_t **parsed_out) {
    if (!alloc || !url || !parsed_out)
        return HU_ERR_INVALID_ARGUMENT;

    *parsed_out = NULL;

    hu_http_response_t hresp = {0};
    hu_error_t err;

    if (extra_headers && extra_headers[0]) {
        err = hu_http_post_json_ex(alloc, url, auth_header, extra_headers, body, body_len, &hresp);
    } else {
        err = hu_http_post_json(alloc, url, auth_header, body, body_len, &hresp);
    }

    if (err != HU_OK)
        return err;

    if (hresp.status_code < 200 || hresp.status_code >= 300) {
        if (hresp.body && hresp.body_len > 0) {
            size_t log_len = hresp.body_len < 500 ? hresp.body_len : 500;
            fprintf(stderr, "[provider_http] HTTP %ld: %.*s\n", hresp.status_code, (int)log_len,
                    hresp.body);
        }
        hu_http_response_free(alloc, &hresp);
        if (hresp.status_code == 401)
            return HU_ERR_PROVIDER_AUTH;
        if (hresp.status_code == 429)
            return HU_ERR_PROVIDER_RATE_LIMITED;
        return HU_ERR_PROVIDER_RESPONSE;
    }

    err = hu_json_parse(alloc, hresp.body, hresp.body_len, parsed_out);
    hu_http_response_free(alloc, &hresp);
    return err;
}
