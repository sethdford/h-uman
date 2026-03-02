#include "seaclaw/core/http.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if SC_IS_TEST
/* In test mode, skip real HTTP and return mock response */
#if 0
static sc_error_t sc_http_get_ex_impl(sc_allocator_t *alloc,
    const char *url,
    const char *extra_headers,
    sc_http_response_t *out)
{
    (void)url;
    (void)extra_headers;
    const char *mock = "{\"status\":\"ok\",\"mock\":\"sc_http_get_ex\"}";
    size_t mock_len = strlen(mock);
    char *body = (char *)alloc->alloc(alloc->ctx, mock_len + 1);
    if (!body) return SC_ERR_OUT_OF_MEMORY;
    memcpy(body, mock, mock_len + 1);
    out->body = body;
    out->body_len = mock_len;
    out->body_cap = mock_len + 1;
    out->status_code = 200;
    out->owned = true;
    return SC_OK;
}
#endif

static sc_error_t sc_http_get_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    sc_http_response_t *out)
{
    if (!alloc || !url || !out) return SC_ERR_INVALID_ARGUMENT;
    (void)auth_header;

    const char *mock = "{\"status\":\"ok\",\"mock\":\"sc_http_get\"}";
    size_t mock_len = strlen(mock);
    char *body = (char *)alloc->alloc(alloc->ctx, mock_len + 1);
    if (!body) return SC_ERR_OUT_OF_MEMORY;
    memcpy(body, mock, mock_len + 1);

    out->body = body;
    out->body_len = mock_len;
    out->body_cap = mock_len + 1;
    out->status_code = 200;
    out->owned = true;
    return SC_OK;
}

static sc_error_t sc_http_post_json_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out)
{
    (void)url;
    (void)auth_header;
    (void)extra_headers;
    (void)json_body;
    (void)json_body_len;

    const char *mock = "{\"choices\":[{\"message\":{\"content\":\"Hello from mock HTTP\"}}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15},"
        "\"model\":\"gpt-4\"}";
    size_t mock_len = strlen(mock);
    char *body = (char *)alloc->alloc(alloc->ctx, mock_len + 1);
    if (!body) return SC_ERR_OUT_OF_MEMORY;
    memcpy(body, mock, mock_len + 1);

    out->body = body;
    out->body_len = mock_len;
    out->body_cap = mock_len + 1;
    out->status_code = 200;
    out->owned = true;
    return SC_OK;
}

#if 0
static sc_error_t sc_http_post_json_stream_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_stream_cb callback,
    void *userdata)
{
    (void)alloc;
    (void)url;
    (void)auth_header;
    (void)extra_headers;
    (void)json_body;
    (void)json_body_len;
    if (!callback) return SC_ERR_INVALID_ARGUMENT;
    const char *sse = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"from \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"mock\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t sse_len = strlen(sse);
    size_t ret = callback(sse, sse_len, userdata);
    if (ret != sse_len) return SC_ERR_IO;
    return SC_OK;
}
#endif
#else
#if defined(SC_HTTP_CURL)
#include <curl/curl.h>

typedef struct write_ctx {
    char *buf;
    size_t len;
    size_t cap;
    sc_allocator_t *alloc;
} write_ctx_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    write_ctx_t *w = (write_ctx_t *)userdata;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    if (w->len + n + 1 > w->cap) {
        size_t new_cap = w->cap ? w->cap * 2 : 4096;
        while (new_cap < w->len + n + 1) new_cap *= 2;
        char *nbuf = (char *)w->alloc->realloc(w->alloc->ctx, w->buf,
            w->cap ? w->cap : 0, new_cap);
        if (!nbuf) return 0;
        w->buf = nbuf;
        w->cap = new_cap;
    }
    memcpy(w->buf + w->len, ptr, n);
    w->len += n;
    w->buf[w->len] = '\0';
    return n;
}

static void add_header(struct curl_slist **list, const char *header) {
    if (header && header[0]) *list = curl_slist_append(*list, header);
}

/* ── Connection pool: reuse curl handles to avoid TCP/TLS handshake ── */
#define SC_CURL_POOL_SIZE 4

static CURL *curl_pool[SC_CURL_POOL_SIZE];
static int curl_pool_count = 0;

static CURL *curl_pool_acquire(void) {
    if (curl_pool_count > 0) {
        CURL *h = curl_pool[--curl_pool_count];
        curl_easy_reset(h);
        return h;
    }
    return curl_easy_init();
}

static void curl_pool_release(CURL *h) {
    if (!h) return;
    if (curl_pool_count < SC_CURL_POOL_SIZE) {
        curl_pool[curl_pool_count++] = h;
    } else {
        curl_easy_cleanup(h);
    }
}

static void curl_setup_common(CURL *curl) {
    /* Default 120s; should be configurable via config. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
}

static sc_error_t sc_http_get_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    sc_http_response_t *out)
{
    if (!alloc || !url || !out) return SC_ERR_INVALID_ARGUMENT;

    CURL *curl = curl_pool_acquire();
    if (!curl) return SC_ERR_NOT_SUPPORTED;

    memset(out, 0, sizeof(*out));

    struct curl_slist *headers = NULL;
    char auth_buf[512];
    if (auth_header && auth_header[0]) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: %s", auth_header);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            add_header(&headers, auth_buf);
    }

    write_ctx_t w = { .buf = NULL, .len = 0, .cap = 0, .alloc = alloc };
    w.buf = (char *)alloc->alloc(alloc->ctx, 4096);
    if (!w.buf) {
        curl_slist_free_all(headers);
        curl_pool_release(curl);
        return SC_ERR_OUT_OF_MEMORY;
    }
    w.cap = 4096;
    w.buf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
    curl_setup_common(curl);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_pool_release(curl);

    if (res != CURLE_OK) {
        alloc->free(alloc->ctx, w.buf, w.cap);
        if (res == CURLE_OPERATION_TIMEDOUT) return SC_ERR_TIMEOUT;
        return SC_ERR_IO;
    }

    out->body = w.buf;
    out->body_len = w.len;
    out->body_cap = w.cap;
    out->status_code = status;
    out->owned = true;
    return SC_OK;
}

static sc_error_t sc_http_get_ex_impl(sc_allocator_t *alloc,
    const char *url,
    const char *extra_headers,
    sc_http_response_t *out)
{
    if (!alloc || !url || !out) return SC_ERR_INVALID_ARGUMENT;

    CURL *curl = curl_pool_acquire();
    if (!curl) return SC_ERR_NOT_SUPPORTED;

    memset(out, 0, sizeof(*out));

    struct curl_slist *headers = NULL;
    if (extra_headers && extra_headers[0]) {
        const char *p = extra_headers;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
            if (linelen > 0 && linelen < 512) {
                char line[512];
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                if (linelen > 0 && line[linelen - 1] == '\r') line[--linelen] = '\0';
                if (linelen > 0) add_header(&headers, line);
            }
            if (!eol) break;
            p = eol + 1;
        }
    }

    write_ctx_t w = { .buf = NULL, .len = 0, .cap = 0, .alloc = alloc };
    w.buf = (char *)alloc->alloc(alloc->ctx, 4096);
    if (!w.buf) {
        curl_slist_free_all(headers);
        curl_pool_release(curl);
        return SC_ERR_OUT_OF_MEMORY;
    }
    w.cap = 4096;
    w.buf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
    curl_setup_common(curl);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_pool_release(curl);

    if (res != CURLE_OK) {
        alloc->free(alloc->ctx, w.buf, w.cap);
        if (res == CURLE_OPERATION_TIMEDOUT) return SC_ERR_TIMEOUT;
        return SC_ERR_IO;
    }

    out->body = w.buf;
    out->body_len = w.len;
    out->body_cap = w.cap;
    out->status_code = status;
    out->owned = true;
    return SC_OK;
}

static sc_error_t sc_http_post_json_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out)
{
    if (!alloc || !url || !out) return SC_ERR_INVALID_ARGUMENT;

    CURL *curl = curl_pool_acquire();
    if (!curl) return SC_ERR_NOT_SUPPORTED;

    memset(out, 0, sizeof(*out));

    struct curl_slist *headers = NULL;
    char auth_buf[512];
    if (auth_header && auth_header[0]) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: %s", auth_header);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            add_header(&headers, auth_buf);
    }
    add_header(&headers, "Content-Type: application/json");
    if (extra_headers && extra_headers[0]) {
        const char *p = extra_headers;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
            if (linelen > 0 && linelen < 512) {
                char line[512];
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                if (line[linelen - 1] == '\r') line[--linelen] = '\0';
                add_header(&headers, line);
            }
            if (!eol) break;
            p = eol + 1;
        }
    }

    write_ctx_t w = { .buf = NULL, .len = 0, .cap = 0, .alloc = alloc };
    w.buf = (char *)alloc->alloc(alloc->ctx, 4096);
    if (!w.buf) {
        curl_slist_free_all(headers);
        curl_pool_release(curl);
        return SC_ERR_OUT_OF_MEMORY;
    }
    w.cap = 4096;
    w.buf[0] = '\0';

    if (json_body_len > (size_t)LONG_MAX) {
        curl_slist_free_all(headers);
        curl_pool_release(curl);
        return SC_ERR_INVALID_ARGUMENT;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json_body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
    curl_setup_common(curl);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_pool_release(curl);

    if (res != CURLE_OK) {
        alloc->free(alloc->ctx, w.buf, w.cap);
        if (res == CURLE_OPERATION_TIMEDOUT) return SC_ERR_TIMEOUT;
        return SC_ERR_IO;
    }

    out->body = w.buf;
    out->body_len = w.len;
    out->body_cap = w.cap;
    out->status_code = status;
    out->owned = true;
    return SC_OK;
}

typedef struct {
    sc_http_stream_cb callback;
    void *userdata;
} stream_ctx_t;

static size_t write_cb_stream(void *ptr, size_t size, size_t nmemb, void *userdata) {
    stream_ctx_t *s = (stream_ctx_t *)userdata;
    size_t n = size * nmemb;
    if (n == 0 || !s->callback) return 0;
    return s->callback((const char *)ptr, n, s->userdata);
}

static sc_error_t sc_http_post_json_stream_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_stream_cb callback,
    void *userdata)
{
    if (!alloc || !url || !callback) return SC_ERR_INVALID_ARGUMENT;

    CURL *curl = curl_pool_acquire();
    if (!curl) return SC_ERR_NOT_SUPPORTED;

    struct curl_slist *headers = NULL;
    char auth_buf[512];
    if (auth_header && auth_header[0]) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: %s", auth_header);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            add_header(&headers, auth_buf);
    }
    add_header(&headers, "Content-Type: application/json");
    if (extra_headers && extra_headers[0]) {
        const char *p = extra_headers;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
            if (linelen > 0 && linelen < 512) {
                char line[512];
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                if (line[linelen - 1] == '\r') line[--linelen] = '\0';
                add_header(&headers, line);
            }
            if (!eol) break;
            p = eol + 1;
        }
    }

    if (json_body_len > (size_t)LONG_MAX) {
        curl_slist_free_all(headers);
        curl_pool_release(curl);
        return SC_ERR_INVALID_ARGUMENT;
    }
    stream_ctx_t ctx = { .callback = callback, .userdata = userdata };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json_body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb_stream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_setup_common(curl);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_pool_release(curl);

    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) return SC_ERR_TIMEOUT;
        return SC_ERR_IO;
    }
    return SC_OK;
}
#else
static sc_error_t sc_http_get_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    sc_http_response_t *out)
{
    (void)alloc;
    (void)url;
    (void)auth_header;
    (void)out;
    return SC_ERR_NOT_SUPPORTED;
}

static sc_error_t sc_http_post_json_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out)
{
    (void)alloc;
    (void)url;
    (void)auth_header;
    (void)extra_headers;
    (void)json_body;
    (void)json_body_len;
    (void)out;
    return SC_ERR_NOT_SUPPORTED;
}

static sc_error_t sc_http_post_json_stream_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_stream_cb callback,
    void *userdata)
{
    (void)alloc;
    (void)url;
    (void)auth_header;
    (void)extra_headers;
    (void)json_body;
    (void)json_body_len;
    (void)callback;
    (void)userdata;
    return SC_ERR_NOT_SUPPORTED;
}
#endif
#endif

sc_error_t sc_http_post_json(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out)
{
    return sc_http_post_json_impl(alloc, url, auth_header, NULL,
        json_body, json_body_len, out);
}

sc_error_t sc_http_post_json_ex(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_response_t *out)
{
    return sc_http_post_json_impl(alloc, url, auth_header, extra_headers,
        json_body, json_body_len, out);
}

sc_error_t sc_http_get(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    sc_http_response_t *out)
{
    return sc_http_get_impl(alloc, url, auth_header, out);
}

#if defined(SC_HTTP_CURL) && !SC_IS_TEST
#include <curl/curl.h>
sc_error_t sc_http_get_ex(sc_allocator_t *alloc,
    const char *url,
    const char *extra_headers,
    sc_http_response_t *out)
{
    return sc_http_get_ex_impl(alloc, url, extra_headers, out);
}

sc_error_t sc_http_request(sc_allocator_t *alloc,
    const char *url,
    const char *method,
    const char *extra_headers,
    const char *body,
    size_t body_len,
    sc_http_response_t *out)
{
    if (!alloc || !url || !method || !out) return SC_ERR_INVALID_ARGUMENT;

    CURL *curl = curl_pool_acquire();
    if (!curl) return SC_ERR_NOT_SUPPORTED;

    memset(out, 0, sizeof(*out));

    struct curl_slist *headers = NULL;
    if (extra_headers && extra_headers[0]) {
        const char *p = extra_headers;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
            if (linelen > 0 && linelen < 512) {
                char line[512];
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                if (linelen > 0 && line[linelen - 1] == '\r') line[--linelen] = '\0';
                if (linelen > 0) add_header(&headers, line);
            }
            if (!eol) break;
            p = eol + 1;
        }
    }

    write_ctx_t w = { .buf = NULL, .len = 0, .cap = 0, .alloc = alloc };
    w.buf = (char *)alloc->alloc(alloc->ctx, 4096);
    if (!w.buf) {
        curl_slist_free_all(headers);
        curl_pool_release(curl);
        return SC_ERR_OUT_OF_MEMORY;
    }
    w.cap = 4096;
    w.buf[0] = '\0';

    if (body && body_len > 0 && body_len > (size_t)LONG_MAX) {
        curl_slist_free_all(headers);
        alloc->free(alloc->ctx, w.buf, w.cap);
        curl_pool_release(curl);
        return SC_ERR_INVALID_ARGUMENT;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
    curl_setup_common(curl);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_pool_release(curl);

    if (res != CURLE_OK) {
        alloc->free(alloc->ctx, w.buf, w.cap);
        if (res == CURLE_OPERATION_TIMEDOUT) return SC_ERR_TIMEOUT;
        return SC_ERR_IO;
    }

    out->body = w.buf;
    out->body_len = w.len;
    out->body_cap = w.cap;
    out->status_code = status;
    out->owned = true;
    return SC_OK;
}
#else
sc_error_t sc_http_get_ex(sc_allocator_t *alloc,
    const char *url,
    const char *extra_headers,
    sc_http_response_t *out)
{
    (void)alloc;(void)url;(void)extra_headers;(void)out;
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_http_request(sc_allocator_t *alloc,
    const char *url,
    const char *method,
    const char *extra_headers,
    const char *body,
    size_t body_len,
    sc_http_response_t *out)
{
    (void)alloc;(void)url;(void)method;(void)extra_headers;(void)body;(void)body_len;(void)out;
    return SC_ERR_NOT_SUPPORTED;
}
#endif

#if defined(SC_HTTP_CURL) && !SC_IS_TEST
sc_error_t sc_http_post_json_stream(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    const char *json_body,
    size_t json_body_len,
    sc_http_stream_cb callback,
    void *userdata)
{
    return sc_http_post_json_stream_impl(alloc, url, auth_header, extra_headers,
        json_body, json_body_len, callback, userdata);
}
#else
sc_error_t sc_http_post_json_stream(sc_allocator_t *alloc,
    const char *url, const char *auth_header, const char *extra_headers,
    const char *json_body, size_t json_body_len,
    sc_http_stream_cb callback, void *userdata)
{
    (void)alloc;(void)url;(void)auth_header;(void)extra_headers;
    (void)json_body;(void)json_body_len;(void)callback;(void)userdata;
    return SC_ERR_NOT_SUPPORTED;
}
#endif

void sc_http_response_free(sc_allocator_t *alloc, sc_http_response_t *resp) {
    if (!resp || !alloc) return;
    if (resp->owned && resp->body) {
        size_t sz = resp->body_cap ? resp->body_cap : resp->body_len + 1;
        alloc->free(alloc->ctx, resp->body, sz);
        resp->body = NULL;
        resp->body_len = 0;
        resp->body_cap = 0;
        resp->owned = false;
    }
}
