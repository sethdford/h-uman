#ifndef SC_HTTP_UTIL_H
#define SC_HTTP_UTIL_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/**
 * HTTP POST via curl subprocess (or libcurl when SC_HTTP_CURL).
 * Caller owns returned body.
 */
sc_error_t sc_http_util_post(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *body, size_t body_len,
    const char *const *headers, size_t header_count,
    char **out_body, size_t *out_len);

/**
 * HTTP GET. Caller owns returned body.
 */
sc_error_t sc_http_util_get(sc_allocator_t *alloc,
    const char *url, size_t url_len,
    const char *const *headers, size_t header_count,
    const char *timeout_secs,
    char **out_body, size_t *out_len);

#endif /* SC_HTTP_UTIL_H */
