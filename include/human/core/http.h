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

/* Maximum serialized request body (bytes) posted to any provider by the JSON
 * POST helpers below. Bodies over this cap are rejected before the request is
 * sent (HU_ERR_INVALID_ARGUMENT), on EVERY path: first attempt, retries, and
 * fallback. This stops a pathological multi-MB body (e.g. a base64 image) from
 * wedging a single-threaded backend via retry amplification. Default 3 MiB;
 * override with the HU_HTTP_MAX_BODY_BYTES environment variable (min 4096). */
size_t hu_http_max_provider_body_bytes(void);

hu_error_t hu_http_post_json(hu_allocator_t *alloc, const char *url,
                             const char *auth_header, /* e.g. "Bearer sk-xxx", or NULL */
                             const char *json_body, size_t json_body_len, hu_http_response_t *out);

/* Variant with extra headers (e.g. "x-api-key: val\r\nanthropic-version: 2023-06-01\r\n") */
hu_error_t hu_http_post_json_ex(hu_allocator_t *alloc, const char *url, const char *auth_header,
                                const char *extra_headers, /* optional, NULL or "Key: value\r\n" */
                                const char *json_body, size_t json_body_len,
                                hu_http_response_t *out);

/* Per-request transport caps. Zero fields resolve to the shared defaults via
 * hu_http_effective_*(): 600 s whole-request (cloud providers, unchanged) and
 * 5 s connect. Local providers pass a shorter timeout_secs so a wedged or
 * half-open loopback upstream (2026-09-03: mlx-server died as a `?E` zombie
 * with the daemon's connection still ESTABLISHED) fails fast enough for the
 * reliable wrapper to route to a cloud fallback instead of stalling the
 * daemon for ten minutes. */
#define HU_HTTP_DEFAULT_TIMEOUT_SECS         600L
#define HU_HTTP_DEFAULT_CONNECT_TIMEOUT_SECS 5L

typedef struct hu_http_request_opts {
    long timeout_secs;         /* whole request; 0 = HU_HTTP_DEFAULT_TIMEOUT_SECS */
    long connect_timeout_secs; /* TCP connect;   0 = HU_HTTP_DEFAULT_CONNECT_TIMEOUT_SECS */
} hu_http_request_opts_t;

/* Resolve the cap a request will actually run under (NULL opts = defaults). */
long hu_http_effective_timeout_secs(const hu_http_request_opts_t *opts);
long hu_http_effective_connect_timeout_secs(const hu_http_request_opts_t *opts);

/* POST JSON with explicit transport caps. opts may be NULL (= defaults).
 * Returns HU_ERR_TIMEOUT when the cap expires before the response arrives. */
hu_error_t hu_http_post_json_opts(hu_allocator_t *alloc, const char *url, const char *auth_header,
                                  const char *extra_headers, const char *json_body,
                                  size_t json_body_len, const hu_http_request_opts_t *opts,
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

/* GET that follows up to max_redirects HTTPS→HTTPS redirects (301/302/307/308).
 * hu_http_get never follows: an API that answers 3xx is a misconfiguration
 * worth surfacing. Feeds are the exception — publishers move RSS URLs and
 * the old one answers 307 forever (openai.com/blog/rss.xml, 2026-09). The
 * Authorization header is not re-sent to a different host. */
hu_error_t hu_http_get_follow(hu_allocator_t *alloc, const char *url,
                              const char *auth_header, /* or NULL */
                              int max_redirects,       /* clamped to [0, 10] */
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
