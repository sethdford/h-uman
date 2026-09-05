#ifndef HU_PROVIDER_HTTP_H
#define HU_PROVIDER_HTTP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Shared HTTP POST + JSON parse for provider chat APIs
 * ────────────────────────────────────────────────────────────────────────── */

/* POST JSON body to url with optional auth and extra headers.
 * On success: *parsed_out is set; caller must hu_json_free(alloc, *parsed_out).
 * auth_header: e.g. "Bearer sk-xxx", or NULL if using extra_headers for auth.
 * extra_headers: optional, e.g. "x-api-key: val\r\nanthropic-version: 2023-06-01\r\n"
 */
hu_error_t hu_provider_http_post_json(hu_allocator_t *alloc, const char *url,
                                      const char *auth_header, const char *extra_headers,
                                      const char *body, size_t body_len,
                                      hu_json_value_t **parsed_out);

/* Same, with explicit transport caps (see hu_http_request_opts_t). opts may
 * be NULL (= shared defaults). HU_ERR_TIMEOUT is returned unchanged so the
 * reliable wrapper can route to a fallback provider. */
hu_error_t hu_provider_http_post_json_opts(hu_allocator_t *alloc, const char *url,
                                           const char *auth_header, const char *extra_headers,
                                           const char *body, size_t body_len,
                                           const hu_http_request_opts_t *opts,
                                           hu_json_value_t **parsed_out);

#endif /* HU_PROVIDER_HTTP_H */
