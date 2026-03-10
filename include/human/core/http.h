#ifndef HU_HTTP_H
#define HU_HTTP_H

#include "allocator.h"
#include "error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_http_response {
    char *body;
    size_t body_len;
    size_t body_cap; /* allocated capacity — must match free size */
    long status_code;
    bool owned; /* if true, caller must free body */
} hu_http_response_t;

hu_error_t hu_http_post_json(hu_allocator_t *alloc, const char *url,
                             const char *auth_header, /* e.g. "Bearer sk-xxx", or NULL */
                             const char *json_body, size_t json_body_len, hu_http_response_t *out);

/* Variant with extra headers (e.g. "x-api-key: val\r\nanthropic-version: 2023-06-01\r\n") */
hu_error_t hu_http_post_json_ex(hu_allocator_t *alloc, const char *url, const char *auth_header,
                                const char *extra_headers, /* optional, NULL or "Key: value\r\n" */
                                const char *json_body, size_t json_body_len,
                                hu_http_response_t *out);

void hu_http_response_free(hu_allocator_t *alloc, hu_http_response_t *resp);

typedef size_t (*hu_http_stream_cb)(const char *chunk, size_t chunk_len, void *userdata);

hu_error_t hu_http_post_json_stream(hu_allocator_t *alloc, const char *url, const char *auth_header,
                                    const char *extra_headers, const char *json_body,
                                    size_t json_body_len, hu_http_stream_cb callback,
                                    void *userdata);

hu_error_t hu_http_get(hu_allocator_t *alloc, const char *url,
                       const char *auth_header, /* e.g. "Bearer sk-xxx", or NULL */
                       hu_http_response_t *out);

/* GET with custom headers (newline-separated: "X-Key: val\nAccept: application/json") */
hu_error_t hu_http_get_ex(hu_allocator_t *alloc, const char *url,
                          const char *extra_headers, /* NULL or "Key: value\n..." */
                          hu_http_response_t *out);

/* Raw HTTP request: method (GET, POST, etc.), optional headers, optional body */
hu_error_t hu_http_request(hu_allocator_t *alloc, const char *url, const char *method,
                           const char *extra_headers, /* NULL or "Key: val\n..." */
                           const char *body, size_t body_len, hu_http_response_t *out);

/* PATCH with JSON body — convenience wrapper around hu_http_request */
hu_error_t hu_http_patch_json(hu_allocator_t *alloc, const char *url, const char *auth_header,
                              const char *json_body, size_t json_body_len, hu_http_response_t *out);

#endif
