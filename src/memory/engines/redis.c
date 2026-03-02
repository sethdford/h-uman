/* Redis memory backend — TCP RESP when SC_ENABLE_REDIS_ENGINE.
 * In SC_IS_TEST: in-memory mock.
 * When SC_ENABLE_REDIS_ENGINE is not set, all operations return SC_ERR_NOT_SUPPORTED.
 * This is intentional, documented stub behavior. */

#include "seaclaw/memory/engines.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(SC_ENABLE_REDIS_ENGINE) && !(defined(SC_IS_TEST) && SC_IS_TEST)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#endif

#define MOCK_MAX_ENTRIES 32
#define REDIS_RECV_BUF 65536

typedef struct mock_entry {
    char *key;
    char *content;
    char *category;
    char *session_id;
} mock_entry_t;

typedef struct sc_redis_memory {
    sc_allocator_t *alloc;
#if defined(SC_ENABLE_REDIS_ENGINE) && !(defined(SC_IS_TEST) && SC_IS_TEST)
    int sock;
    char *host;
    unsigned short port;
    char *key_prefix;
#endif
#if defined(SC_IS_TEST) && SC_IS_TEST
    mock_entry_t entries[MOCK_MAX_ENTRIES];
    size_t entry_count;
#endif
} sc_redis_memory_t;

__attribute__((unused))
static const char *category_to_string(const sc_memory_category_t *cat) {
    if (!cat) return "core";
    switch (cat->tag) {
        case SC_MEMORY_CATEGORY_CORE: return "core";
        case SC_MEMORY_CATEGORY_DAILY: return "daily";
        case SC_MEMORY_CATEGORY_CONVERSATION: return "conversation";
        case SC_MEMORY_CATEGORY_CUSTOM:
            if (cat->data.custom.name && cat->data.custom.name_len > 0)
                return cat->data.custom.name;
            return "custom";
        default: return "core";
    }
}

#if defined(SC_IS_TEST) && SC_IS_TEST
static void mock_free_entry(sc_allocator_t *alloc, mock_entry_t *e) {
    if (!alloc || !e) return;
    if (e->key) alloc->free(alloc->ctx, e->key, strlen(e->key) + 1);
    if (e->content) alloc->free(alloc->ctx, e->content, strlen(e->content) + 1);
    if (e->category) alloc->free(alloc->ctx, e->category, strlen(e->category) + 1);
    if (e->session_id) alloc->free(alloc->ctx, e->session_id, strlen(e->session_id) + 1);
    e->key = e->content = e->category = e->session_id = NULL;
}

static mock_entry_t *mock_find_by_key(sc_redis_memory_t *self,
    const char *key, size_t key_len) {
    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

static int mock_contains_substring(const char *haystack, size_t hlen,
    const char *needle, size_t nlen) {
    if (nlen == 0) return 1;
    if (hlen < nlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0) return 1;
    }
    return 0;
}
#endif /* SC_IS_TEST */

#if defined(SC_ENABLE_REDIS_ENGINE) && !(defined(SC_IS_TEST) && SC_IS_TEST)
static int redis_connect(sc_redis_memory_t *self) {
    if (self->sock >= 0) return 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)self->port);
    if (getaddrinfo(self->host, port_str, &hints, &res) != 0) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return -1; }
    if (connect(s, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        close(s);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    self->sock = s;
    return 0;
}

static int redis_send_array(int sock, const char **parts, int count) {
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "*%d\r\n", count);
    if (send(sock, hdr, (size_t)n, 0) != (ssize_t)n) return -1;
    for (int i = 0; i < count; i++) {
        size_t plen = strlen(parts[i]);
        char pbuf[256];
        int pn = snprintf(pbuf, sizeof(pbuf), "$%zu\r\n%s\r\n", plen, parts[i]);
        if (pn <= 0 || (size_t)pn >= sizeof(pbuf)) return -1;
        if (send(sock, pbuf, (size_t)pn, 0) != (ssize_t)pn) return -1;
    }
    return 0;
}

static int redis_send_array_start(int sock, int count) {
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "*%d\r\n", count);
    if (n <= 0 || (size_t)n >= sizeof(hdr)) return -1;
    return (send(sock, hdr, (size_t)n, 0) == (ssize_t)n) ? 0 : -1;
}

static int redis_send_bulk(int sock, const char *str, size_t len) {
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    if (n <= 0 || (size_t)n >= sizeof(hdr)) return -1;
    if (send(sock, hdr, (size_t)n, 0) != (ssize_t)n) return -1;
    if (len > 0 && send(sock, str, len, 0) != (ssize_t)len) return -1;
    if (send(sock, "\r\n", 2, 0) != 2) return -1;
    return 0;
}

static sc_error_t redis_recv_reply(int sock, char *out_buf, size_t out_cap, size_t *out_len) {
    *out_len = 0;
    char buf[REDIS_RECV_BUF];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return SC_ERR_MEMORY_BACKEND;
    buf[n] = '\0';
    size_t copy = (size_t)n < out_cap ? (size_t)n : out_cap - 1;
    memcpy(out_buf, buf, copy);
    out_buf[copy] = '\0';
    *out_len = copy;
    return SC_OK;
}

/* Parse HGETALL reply: *N\r\n $len\r\nfield\r\n $len\r\nvalue\r\n ... Return hash as key-value pairs. */
typedef struct redis_hgetall_ctx {
    char *content;
    size_t content_len;
    char *category;
    size_t category_len;
    char *session_id;
    size_t session_id_len;
    char *key;
    size_t key_len;
} redis_hgetall_ctx_t;

static sc_error_t redis_parse_hgetall(const char *reply, size_t reply_len,
    redis_hgetall_ctx_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = reply;
    const char *end = reply + reply_len;
    if (p >= end || *p != '*') return SC_ERR_PARSE;
    p++;
    long n = strtol(p, (char **)&p, 10);
    if (n <= 0 || (n % 2) != 0) return SC_ERR_PARSE;
    while (p < end && (*p == '\r' || *p == '\n')) p++;
    for (long i = 0; i < n && p < end; i += 2) {
        if (*p != '$') return SC_ERR_PARSE;
        p++;
        long flen = strtol(p, (char **)&p, 10);
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        const char *fstart = p;
        p += flen;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (*p != '$') return SC_ERR_PARSE;
        p++;
        long vlen = strtol(p, (char **)&p, 10);
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        const char *vstart = p;
        p += vlen;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (flen == 7 && memcmp(fstart, "content", 7) == 0) {
            out->content = (char *)vstart;
            out->content_len = (size_t)vlen;
        } else if (flen == 8 && memcmp(fstart, "category", 8) == 0) {
            out->category = (char *)vstart;
            out->category_len = (size_t)vlen;
        } else if (flen == 10 && memcmp(fstart, "session_id", 10) == 0) {
            out->session_id = (char *)vstart;
            out->session_id_len = (size_t)vlen;
        } else if (flen == 3 && memcmp(fstart, "key", 3) == 0) {
            out->key = (char *)vstart;
            out->key_len = (size_t)vlen;
        }
    }
    return SC_OK;
}
#endif

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "redis";
}

static sc_error_t impl_store(void *ctx,
    const char *key, size_t key_len,
    const char *content, size_t content_len,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    sc_allocator_t *alloc = self->alloc;
    mock_entry_t *existing = mock_find_by_key(self, key, key_len);
    const char *cat_str = category_to_string(category);

    if (existing) {
        if (existing->content) alloc->free(alloc->ctx, existing->content, strlen(existing->content) + 1);
        if (existing->category) alloc->free(alloc->ctx, existing->category, strlen(existing->category) + 1);
        if (existing->session_id) alloc->free(alloc->ctx, existing->session_id, strlen(existing->session_id) + 1);
        existing->content = sc_strndup(alloc, content, content_len);
        existing->category = sc_strndup(alloc, cat_str, strlen(cat_str));
        existing->session_id = (session_id && session_id_len > 0)
            ? sc_strndup(alloc, session_id, session_id_len) : NULL;
        return existing->content ? SC_OK : SC_ERR_OUT_OF_MEMORY;
    }

    if (self->entry_count >= MOCK_MAX_ENTRIES) return SC_ERR_OUT_OF_MEMORY;
    mock_entry_t *e = &self->entries[self->entry_count];
    e->key = sc_strndup(alloc, key, key_len);
    e->content = sc_strndup(alloc, content, content_len);
    e->category = sc_strndup(alloc, cat_str, strlen(cat_str));
    e->session_id = (session_id && session_id_len > 0)
        ? sc_strndup(alloc, session_id, session_id_len) : NULL;
    if (!e->key || !e->content || !e->category) {
        mock_free_entry(alloc, e);
        return SC_ERR_OUT_OF_MEMORY;
    }
    self->entry_count++;
    return SC_OK;

#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    if (redis_connect(self) != 0) return SC_ERR_MEMORY_BACKEND;

    const char *cat_str = category_to_string(category);
    const char *sid = (session_id && session_id_len > 0) ? session_id : "";
    size_t sid_len = (session_id && session_id_len > 0) ? session_id_len : 0;

    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%lu", (unsigned long)time(NULL));
    size_t ts_len = strlen(ts_buf);

    char id_buf[64];
    snprintf(id_buf, sizeof(id_buf), "%lu-%u", (unsigned long)time(NULL), (unsigned)rand());
    size_t id_len = strlen(id_buf);

    char entry_key[512];
    int ek = snprintf(entry_key, sizeof(entry_key), "%s:entry:%.*s",
        self->key_prefix ? self->key_prefix : "mem", (int)key_len, key);
    if (ek <= 0 || (size_t)ek >= sizeof(entry_key)) return SC_ERR_INVALID_ARGUMENT;

    if (redis_send_array_start(self->sock, 16) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "HSET", 4) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, entry_key, (size_t)ek) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "id", 2) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, id_buf, id_len) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "content", 7) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, content, content_len) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "category", 8) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, cat_str, strlen(cat_str)) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "session_id", 10) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, sid, sid_len) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "created_at", 10) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, ts_buf, ts_len) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "updated_at", 10) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, ts_buf, ts_len) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "key", 3) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, key, key_len) != 0) return SC_ERR_MEMORY_STORE;

    char reply[128];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK)
        return SC_ERR_MEMORY_STORE;

    char keys_key[256];
    snprintf(keys_key, sizeof(keys_key), "%s:keys",
        self->key_prefix ? self->key_prefix : "mem");
    if (redis_send_array_start(self->sock, 2) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "SADD", 4) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, keys_key, strlen(keys_key)) != 0) return SC_ERR_MEMORY_STORE;
    redis_recv_reply(self->sock, reply, sizeof(reply), &rlen);

    char cat_key[256];
    snprintf(cat_key, sizeof(cat_key), "%s:cat:%s",
        self->key_prefix ? self->key_prefix : "mem", cat_str);
    if (redis_send_array_start(self->sock, 3) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, "SADD", 4) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, cat_key, strlen(cat_key)) != 0) return SC_ERR_MEMORY_STORE;
    if (redis_send_bulk(self->sock, key, key_len) != 0) return SC_ERR_MEMORY_STORE;
    redis_recv_reply(self->sock, reply, sizeof(reply), &rlen);

    if (session_id && session_id_len > 0) {
        char sess_key[512];
        snprintf(sess_key, sizeof(sess_key), "%s:sessions:%.*s",
            self->key_prefix ? self->key_prefix : "mem", (int)session_id_len, session_id);
        if (redis_send_array_start(self->sock, 3) != 0) return SC_ERR_MEMORY_STORE;
        if (redis_send_bulk(self->sock, "SADD", 4) != 0) return SC_ERR_MEMORY_STORE;
        if (redis_send_bulk(self->sock, sess_key, strlen(sess_key)) != 0) return SC_ERR_MEMORY_STORE;
        if (redis_send_bulk(self->sock, key, key_len) != 0) return SC_ERR_MEMORY_STORE;
        redis_recv_reply(self->sock, reply, sizeof(reply), &rlen);
    }

    return SC_OK;
#else
    (void)ctx;(void)key;(void)key_len;(void)content;(void)content_len;
    (void)category;(void)session_id;(void)session_id_len;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len, size_t limit,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    if (query_len == 0) return SC_OK;  /* empty query returns no matches */
    size_t cap = 0, n = 0;
    sc_memory_entry_t *results = NULL;

    for (size_t i = 0; i < self->entry_count && n < limit; i++) {
        mock_entry_t *e = &self->entries[i];
        if (!e->key) continue;
        bool match = mock_contains_substring(e->key, strlen(e->key), query, query_len)
            || mock_contains_substring(e->content, strlen(e->content), query, query_len);
        if (session_id && session_id_len > 0 && e->session_id) {
            if (strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                match = false;
        } else if (session_id && session_id_len > 0 && !e->session_id)
            match = false;
        if (!match) continue;

        if (n >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            sc_memory_entry_t *tmp = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!tmp) {
                for (size_t j = 0; j < n; j++) sc_memory_entry_free_fields(alloc, &results[j]);
                if (results) alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            results = tmp;
            cap = new_cap;
        }

        sc_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = r->key = sc_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->id_len = r->key_len;
        r->content = sc_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        r->timestamp = sc_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (e->session_id) {
            r->session_id = sc_strndup(alloc, e->session_id, strlen(e->session_id));
            r->session_id_len = strlen(e->session_id);
        }
        r->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        r->category.data.custom.name = sc_strndup(alloc, e->category, strlen(e->category));
        r->category.data.custom.name_len = strlen(e->category);
        if (!r->key || !r->content) {
            for (size_t j = 0; j <= n; j++) sc_memory_entry_free_fields(alloc, &results[j]);
            alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
            return SC_ERR_OUT_OF_MEMORY;
        }
        n++;
    }
    *out = results;
    *out_count = n;
    return SC_OK;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    if (redis_connect(self) != 0) return SC_ERR_MEMORY_BACKEND;
    char keys_key[256];
    snprintf(keys_key, sizeof(keys_key), "%s:keys",
        self->key_prefix ? self->key_prefix : "mem");
    if (redis_send_array_start(self->sock, 2) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, "SMEMBERS", 8) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, keys_key, strlen(keys_key)) != 0) return SC_ERR_MEMORY_BACKEND;
    char reply[REDIS_RECV_BUF];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK)
        return SC_ERR_MEMORY_RECALL;
    const char *p = reply;
    if (*p != '*' || rlen < 4) { *out = NULL; *out_count = 0; return SC_OK; }
    p++;
    long key_count = strtol(p, (char **)&p, 10);
    if (key_count <= 0) { *out = NULL; *out_count = 0; return SC_OK; }
    struct { const char *ptr; size_t len; } keys[64];
    long nkeys = key_count < 64 ? key_count : 64;
    for (long ki = 0; ki < nkeys; ki++) {
        while (p < reply + rlen && (*p == '\r' || *p == '\n')) p++;
        if (p >= reply + rlen || *p != '$') { nkeys = ki; break; }
        p++;
        long klen = strtol(p, (char **)&p, 10);
        while (p < reply + rlen && (*p == '\r' || *p == '\n')) p++;
        keys[ki].ptr = p;
        keys[ki].len = (size_t)klen;
        p += klen;
    }
    size_t cap = 4, n = 0;
    sc_memory_entry_t *results = (sc_memory_entry_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(sc_memory_entry_t));
    if (!results) return SC_ERR_OUT_OF_MEMORY;
    memset(results, 0, cap * sizeof(sc_memory_entry_t));
    for (long ki = 0; ki < nkeys && n < limit; ki++) {
        const char *kstart = keys[ki].ptr;
        size_t klen = keys[ki].len;
        char entry_key[512];
        snprintf(entry_key, sizeof(entry_key), "%s:entry:%.*s",
            self->key_prefix ? self->key_prefix : "mem", (int)klen, kstart);
        if (redis_send_array_start(self->sock, 2) != 0) break;
        if (redis_send_bulk(self->sock, "HGETALL", 7) != 0) break;
        if (redis_send_bulk(self->sock, entry_key, strlen(entry_key)) != 0) break;
        char hr[2048];
        size_t hrlen;
        if (redis_recv_reply(self->sock, hr, sizeof(hr), &hrlen) != SC_OK) break;
        redis_hgetall_ctx_t hc;
        if (redis_parse_hgetall(hr, hrlen, &hc) != SC_OK) continue;
        if (session_id && session_id_len > 0 &&
            (!hc.session_id || hc.session_id_len != session_id_len ||
             memcmp(hc.session_id, session_id, session_id_len) != 0))
            continue;
        bool match = (query_len == 0);
        if (!match && klen >= query_len &&
            memmem(kstart, klen, query, query_len) != NULL)
            match = true;
        if (!match && hc.content && hc.content_len >= query_len &&
            memmem(hc.content, hc.content_len, query, query_len) != NULL)
            match = true;
        if (!match) continue;
        if (n >= cap) {
            size_t new_cap = cap * 2;
            sc_memory_entry_t *tmp = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!tmp) break;
            results = tmp;
            cap = new_cap;
        }
        sc_memory_entry_t *r = &results[n];
        r->id = r->key = sc_strndup(alloc, hc.key ? hc.key : kstart, hc.key ? hc.key_len : klen);
        r->key_len = r->id_len = hc.key ? hc.key_len : klen;
        r->content = sc_strndup(alloc, hc.content ? hc.content : "", hc.content ? hc.content_len : 0);
        r->content_len = hc.content ? hc.content_len : 0;
        r->timestamp = sc_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (hc.session_id) {
            r->session_id = sc_strndup(alloc, hc.session_id, hc.session_id_len);
            r->session_id_len = hc.session_id_len;
        }
        r->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        if (hc.category) {
            r->category.data.custom.name = sc_strndup(alloc, hc.category, hc.category_len);
            r->category.data.custom.name_len = hc.category_len;
        }
        n++;
    }
    *out = results;
    *out_count = n;
    return SC_OK;
#else
    (void)ctx;(void)alloc;(void)query;(void)query_len;(void)limit;
    (void)session_id;(void)session_id_len;
    *out = NULL;
    *out_count = 0;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc,
    const char *key, size_t key_len, sc_memory_entry_t *out, bool *found) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    mock_entry_t *e = mock_find_by_key(self, key, key_len);
    *found = false;
    memset(out, 0, sizeof(*out));
    if (!e) return SC_OK;
    *found = true;
    out->id = out->key = sc_strndup(alloc, e->key, strlen(e->key));
    out->key_len = strlen(e->key);
    out->id_len = out->key_len;
    out->content = sc_strndup(alloc, e->content, strlen(e->content));
    out->content_len = strlen(e->content);
    out->timestamp = sc_sprintf(alloc, "0");
    out->timestamp_len = out->timestamp ? strlen(out->timestamp) : 0;
    if (e->session_id) {
        out->session_id = sc_strndup(alloc, e->session_id, strlen(e->session_id));
        out->session_id_len = strlen(e->session_id);
    }
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name = sc_strndup(alloc, e->category, strlen(e->category));
    out->category.data.custom.name_len = strlen(e->category);
    return SC_OK;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *found = false;
    memset(out, 0, sizeof(*out));
    if (redis_connect(self) != 0) return SC_ERR_MEMORY_BACKEND;
    char entry_key[512];
    snprintf(entry_key, sizeof(entry_key), "%s:entry:%.*s",
        self->key_prefix ? self->key_prefix : "mem", (int)key_len, key);
    if (redis_send_array_start(self->sock, 2) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, "HGETALL", 7) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, entry_key, strlen(entry_key)) != 0) return SC_ERR_MEMORY_BACKEND;
    char reply[2048];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK)
        return SC_ERR_MEMORY_RECALL;
    redis_hgetall_ctx_t hc;
    if (redis_parse_hgetall(reply, rlen, &hc) != SC_OK) return SC_OK;
    if (!hc.content && !hc.key) return SC_OK;
    *found = true;
    out->id = out->key = sc_strndup(alloc, hc.key ? hc.key : key, hc.key ? hc.key_len : key_len);
    out->key_len = out->id_len = hc.key ? hc.key_len : key_len;
    out->content = sc_strndup(alloc, hc.content ? hc.content : "", hc.content ? hc.content_len : 0);
    out->content_len = hc.content ? hc.content_len : 0;
    out->timestamp = sc_sprintf(alloc, "0");
    out->timestamp_len = out->timestamp ? strlen(out->timestamp) : 0;
    if (hc.session_id) {
        out->session_id = sc_strndup(alloc, hc.session_id, hc.session_id_len);
        out->session_id_len = hc.session_id_len;
    }
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    if (hc.category) {
        out->category.data.custom.name = sc_strndup(alloc, hc.category, hc.category_len);
        out->category.data.custom.name_len = hc.category_len;
    }
    return SC_OK;
#else
    (void)ctx;(void)alloc;(void)key;(void)key_len;(void)out;
    *found = false;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    const char *cat_filter = category ? category_to_string(category) : NULL;
    *out = NULL;
    *out_count = 0;
    size_t cap = 0, n = 0;
    sc_memory_entry_t *results = NULL;

    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (!e->key) continue;
        if (cat_filter && (!e->category || strcmp(e->category, cat_filter) != 0))
            continue;
        if (session_id && session_id_len > 0) {
            if (!e->session_id) continue;
            if (strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                continue;
        }

        if (n >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            sc_memory_entry_t *tmp = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!tmp) {
                for (size_t j = 0; j < n; j++) sc_memory_entry_free_fields(alloc, &results[j]);
                if (results) alloc->free(alloc->ctx, results, cap * sizeof(sc_memory_entry_t));
                return SC_ERR_OUT_OF_MEMORY;
            }
            results = tmp;
            cap = new_cap;
        }

        sc_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = r->key = sc_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->id_len = r->key_len;
        r->content = sc_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        r->timestamp = sc_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (e->session_id) {
            r->session_id = sc_strndup(alloc, e->session_id, strlen(e->session_id));
            r->session_id_len = strlen(e->session_id);
        }
        r->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        r->category.data.custom.name = sc_strndup(alloc, e->category, strlen(e->category));
        r->category.data.custom.name_len = strlen(e->category);
        n++;
    }
    *out = results;
    *out_count = n;
    return SC_OK;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    const char *cat_filter = category ? category_to_string(category) : NULL;
    *out = NULL;
    *out_count = 0;
    if (redis_connect(self) != 0) return SC_ERR_MEMORY_BACKEND;
    char keys_key[256];
    snprintf(keys_key, sizeof(keys_key), "%s:keys",
        self->key_prefix ? self->key_prefix : "mem");
    if (redis_send_array_start(self->sock, 2) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, "SMEMBERS", 8) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, keys_key, strlen(keys_key)) != 0) return SC_ERR_MEMORY_BACKEND;
    char reply[REDIS_RECV_BUF];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK)
        return SC_ERR_MEMORY_RECALL;
    const char *p = reply;
    if (*p != '*' || rlen < 4) return SC_OK;
    p++;
    long key_count = strtol(p, (char **)&p, 10);
    if (key_count <= 0) return SC_OK;
    struct { const char *ptr; size_t len; } keys[64];
    long nkeys = key_count < 64 ? key_count : 64;
    for (long ki = 0; ki < nkeys; ki++) {
        while (p < reply + rlen && (*p == '\r' || *p == '\n')) p++;
        if (p >= reply + rlen || *p != '$') { nkeys = ki; break; }
        p++;
        long klen = strtol(p, (char **)&p, 10);
        while (p < reply + rlen && (*p == '\r' || *p == '\n')) p++;
        keys[ki].ptr = p;
        keys[ki].len = (size_t)klen;
        p += klen;
    }
    size_t cap = 4, n = 0;
    sc_memory_entry_t *results = (sc_memory_entry_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(sc_memory_entry_t));
    if (!results) return SC_ERR_OUT_OF_MEMORY;
    memset(results, 0, cap * sizeof(sc_memory_entry_t));
    for (long ki = 0; ki < nkeys; ki++) {
        const char *kstart = keys[ki].ptr;
        size_t klen = keys[ki].len;
        char entry_key[512];
        snprintf(entry_key, sizeof(entry_key), "%s:entry:%.*s",
            self->key_prefix ? self->key_prefix : "mem", (int)klen, kstart);
        if (redis_send_array_start(self->sock, 2) != 0) break;
        if (redis_send_bulk(self->sock, "HGETALL", 7) != 0) break;
        if (redis_send_bulk(self->sock, entry_key, strlen(entry_key)) != 0) break;
        char hr[2048];
        size_t hrlen;
        if (redis_recv_reply(self->sock, hr, sizeof(hr), &hrlen) != SC_OK) break;
        redis_hgetall_ctx_t hc;
        if (redis_parse_hgetall(hr, hrlen, &hc) != SC_OK) continue;
        if (cat_filter && (!hc.category || hc.category_len != strlen(cat_filter) ||
            memcmp(hc.category, cat_filter, strlen(cat_filter)) != 0))
            continue;
        if (session_id && session_id_len > 0 &&
            (!hc.session_id || hc.session_id_len != session_id_len ||
             memcmp(hc.session_id, session_id, session_id_len) != 0))
            continue;
        if (n >= cap) {
            size_t new_cap = cap * 2;
            sc_memory_entry_t *tmp = (sc_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(sc_memory_entry_t),
                new_cap * sizeof(sc_memory_entry_t));
            if (!tmp) break;
            results = tmp;
            cap = new_cap;
        }
        sc_memory_entry_t *r = &results[n];
        r->id = r->key = sc_strndup(alloc, hc.key ? hc.key : kstart, hc.key ? hc.key_len : klen);
        r->key_len = r->id_len = hc.key ? hc.key_len : klen;
        r->content = sc_strndup(alloc, hc.content ? hc.content : "", hc.content ? hc.content_len : 0);
        r->content_len = hc.content ? hc.content_len : 0;
        r->timestamp = sc_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (hc.session_id) {
            r->session_id = sc_strndup(alloc, hc.session_id, hc.session_id_len);
            r->session_id_len = hc.session_id_len;
        }
        r->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        if (hc.category) {
            r->category.data.custom.name = sc_strndup(alloc, hc.category, hc.category_len);
            r->category.data.custom.name_len = hc.category_len;
        }
        n++;
    }
    *out = results;
    *out_count = n;
    return SC_OK;
#else
    (void)ctx;(void)alloc;(void)category;(void)session_id;(void)session_id_len;
    *out = NULL;
    *out_count = 0;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *deleted = false;
    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0) {
            mock_free_entry(self->alloc, e);
            memmove(&self->entries[i], &self->entries[i + 1],
                (self->entry_count - i - 1) * sizeof(mock_entry_t));
            memset(&self->entries[self->entry_count - 1], 0, sizeof(mock_entry_t));
            self->entry_count--;
            *deleted = true;
            return SC_OK;
        }
    }
    return SC_OK;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *deleted = false;
    if (redis_connect(self) != 0) return SC_ERR_MEMORY_BACKEND;
    char entry_key[512];
    snprintf(entry_key, sizeof(entry_key), "%s:entry:%.*s",
        self->key_prefix ? self->key_prefix : "mem", (int)key_len, key);
    char keys_key[256];
    snprintf(keys_key, sizeof(keys_key), "%s:keys",
        self->key_prefix ? self->key_prefix : "mem");
    if (redis_send_array_start(self->sock, 2) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, "DEL", 3) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, entry_key, strlen(entry_key)) != 0) return SC_ERR_MEMORY_BACKEND;
    char reply[128];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK) return SC_ERR_MEMORY_STORE;
    if (reply[0] == ':') { long v = strtol(reply + 1, NULL, 10); *deleted = (v > 0); }
    if (redis_send_array_start(self->sock, 3) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, "SREM", 4) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, keys_key, strlen(keys_key)) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, key, key_len) != 0) return SC_ERR_MEMORY_BACKEND;
    redis_recv_reply(self->sock, reply, sizeof(reply), &rlen);
    return SC_OK;
#else
    (void)ctx;(void)key;(void)key_len;
    *deleted = false;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static sc_error_t impl_count(void *ctx, size_t *out) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *out = self->entry_count;
    return SC_OK;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    *out = 0;
    if (redis_connect(self) != 0) return SC_ERR_MEMORY_BACKEND;
    char keys_key[256];
    snprintf(keys_key, sizeof(keys_key), "%s:keys",
        self->key_prefix ? self->key_prefix : "mem");
    if (redis_send_array_start(self->sock, 2) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, "SCARD", 5) != 0) return SC_ERR_MEMORY_BACKEND;
    if (redis_send_bulk(self->sock, keys_key, strlen(keys_key)) != 0) return SC_ERR_MEMORY_BACKEND;
    char reply[128];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK) return SC_ERR_MEMORY_BACKEND;
    if (reply[0] == ':') *out = (size_t)strtoul(reply + 1, NULL, 10);
    return SC_OK;
#else
    (void)ctx;
    *out = 0;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

static bool impl_health_check(void *ctx) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)ctx;
    return true;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    if (redis_connect(self) != 0) return false;
    if (redis_send_array_start(self->sock, 1) != 0) return false;
    if (redis_send_bulk(self->sock, "PING", 4) != 0) return false;
    char reply[64];
    size_t rlen;
    if (redis_recv_reply(self->sock, reply, sizeof(reply), &rlen) != SC_OK) return false;
    return (rlen >= 5 && memcmp(reply, "+PONG", 5) == 0);
#else
    (void)ctx;
    return false;
#endif
}

static void impl_deinit(void *ctx) {
    sc_redis_memory_t *self = (sc_redis_memory_t *)ctx;
    if (!self || !self->alloc) return;
#if defined(SC_IS_TEST) && SC_IS_TEST
    for (size_t i = 0; i < self->entry_count; i++)
        mock_free_entry(self->alloc, &self->entries[i]);
    self->entry_count = 0;
#elif defined(SC_ENABLE_REDIS_ENGINE)
    if (self->sock >= 0) { close(self->sock); self->sock = -1; }
    if (self->host) { self->alloc->free(self->alloc->ctx, self->host, strlen(self->host) + 1); self->host = NULL; }
    if (self->key_prefix) { self->alloc->free(self->alloc->ctx, self->key_prefix, strlen(self->key_prefix) + 1); self->key_prefix = NULL; }
#endif
    self->alloc->free(self->alloc->ctx, self, sizeof(sc_redis_memory_t));
}

static const sc_memory_vtable_t redis_vtable = {
    .name = impl_name,
    .store = impl_store,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

sc_memory_t sc_redis_memory_create(sc_allocator_t *alloc,
    const char *host, unsigned short port, const char *key_prefix) {
    if (!alloc) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    sc_redis_memory_t *self = (sc_redis_memory_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_redis_memory_t));
    if (!self) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    memset(self, 0, sizeof(sc_redis_memory_t));
    self->alloc = alloc;
#if defined(SC_IS_TEST) && SC_IS_TEST
    (void)host;(void)port;(void)key_prefix;
    return (sc_memory_t){ .ctx = self, .vtable = &redis_vtable };
#elif defined(SC_ENABLE_REDIS_ENGINE)
    self->sock = -1;
    self->host = host ? sc_strdup(alloc, host) : NULL;
    self->port = port ? port : 6379;
    self->key_prefix = key_prefix ? sc_strdup(alloc, key_prefix) : sc_strdup(alloc, "mem");
    if (!self->host) self->host = sc_strdup(alloc, "localhost");
    if (!self->host || !self->key_prefix) {
        if (self->host) alloc->free(alloc->ctx, self->host, strlen(self->host) + 1);
        if (self->key_prefix) alloc->free(alloc->ctx, self->key_prefix, strlen(self->key_prefix) + 1);
        alloc->free(alloc->ctx, self, sizeof(sc_redis_memory_t));
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }
    return (sc_memory_t){ .ctx = self, .vtable = &redis_vtable };
#else
    (void)host;(void)port;(void)key_prefix;
    return (sc_memory_t){ .ctx = self, .vtable = &redis_vtable };
#endif
}
