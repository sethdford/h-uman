#ifndef HU_HTTP_UTIL_H
#define HU_HTTP_UTIL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/**
 * HTTP POST via curl subprocess (or libcurl when HU_HTTP_CURL).
 * Caller owns returned body.
 */
hu_error_t hu_http_util_post(hu_allocator_t *alloc, const char *url, size_t url_len,
                             const char *body, size_t body_len, const char *const *headers,
                             size_t header_count, char **out_body, size_t *out_len);

/**
 * HTTP GET. Caller owns returned body.
 */
hu_error_t hu_http_util_get(hu_allocator_t *alloc, const char *url, size_t url_len,
                            const char *const *headers, size_t header_count,
                            const char *timeout_secs, char **out_body, size_t *out_len);

#endif /* HU_HTTP_UTIL_H */
