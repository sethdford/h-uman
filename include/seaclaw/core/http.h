#ifndef SC_HTTP_H
#define SC_HTTP_H

#include "allocator.h"
#include "error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct sc_http_response {
    char *body;
    size_t body_len;
    size_t body_cap;   /* allocated capacity — must match free size */
    long status_code;
    bool owned;  /* if true, caller must free body */
} sc_http_response_t;

sc_error_t sc_http_post_json(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,   /* e.g. "Bearer sk-xxx", or NULL */
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out);

/* Variant with extra headers (e.g. "x-api-key: val\r\nanthropic-version: 2023-06-01\r\n") */
sc_error_t sc_http_post_json_ex(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers, /* optional, NULL or "Key: value\r\n" */
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out);

void sc_http_response_free(sc_allocator_t *alloc, sc_http_response_t *resp);

typedef size_t (*sc_http_stream_cb)(const char *chunk, size_t chunk_len, void *userdata);

sc_error_t sc_http_post_json_stream(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_stream_cb callback,
    void *userdata);

sc_error_t sc_http_get(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,   /* e.g. "Bearer sk-xxx", or NULL */
    sc_http_response_t *out);

/* GET with custom headers (newline-separated: "X-Key: val\nAccept: application/json") */
sc_error_t sc_http_get_ex(sc_allocator_t *alloc,
    const char *url,
    const char *extra_headers, /* NULL or "Key: value\n..." */
    sc_http_response_t *out);

/* Raw HTTP request: method (GET, POST, etc.), optional headers, optional body */
sc_error_t sc_http_request(sc_allocator_t *alloc,
    const char *url,
    const char *method,
    const char *extra_headers, /* NULL or "Key: val\n..." */
    const char *body,
    size_t body_len,
    sc_http_response_t *out);

#endif
