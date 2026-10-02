#include "human/websocket/websocket.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/local_only_guard.h"
#include "human/core/log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef HU_HAS_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define HU_WS_MAGIC             "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define HU_WS_MAX_FRAME_PAYLOAD (4 * 1024 * 1024)
#define HU_WS_MAX_MSG           (64 * 1024)

/* RFC 6455 opcodes */
#define HU_WS_OP_CONTINUATION 0
#define HU_WS_OP_TEXT         1
#define HU_WS_OP_BINARY       2
#define HU_WS_OP_CLOSE        8
#define HU_WS_OP_PING         9
#define HU_WS_OP_PONG         10

#define HU_WS_INVALID_SOCK (-1)

struct hu_ws_client {
    hu_allocator_t *alloc;
#if defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
    int sockfd;
#ifdef HU_HAS_TLS
    SSL *ssl;
    SSL_CTX *ssl_ctx;
#endif
    char *frag_buf;
    size_t frag_len;
    size_t frag_cap;
    unsigned frag_opcode;
#endif
};

/* Build a masked WebSocket frame into buf. Returns bytes written. */
size_t hu_ws_build_frame(char *buf, size_t buf_size, unsigned opcode, const char *payload,
                         size_t payload_len, const unsigned char mask_key[4]) {
    if (buf_size < 6 || payload_len > HU_WS_MAX_FRAME_PAYLOAD)
        return 0;

    size_t pos = 0;
    buf[pos++] = (char)(0x80 | (opcode & 0x0F)); /* FIN=1, opcode */

    if (payload_len <= 125) {
        buf[pos++] = (char)(0x80 | (payload_len & 0x7F)); /* MASK=1, len */
    } else if (payload_len <= 65535) {
        buf[pos++] = (char)(0x80 | 126);
        buf[pos++] = (char)((payload_len >> 8) & 0xFF);
        buf[pos++] = (char)(payload_len & 0xFF);
    } else {
        buf[pos++] = (char)(0x80 | 127);
        buf[pos++] = (char)((payload_len >> 56) & 0xFF);
        buf[pos++] = (char)((payload_len >> 48) & 0xFF);
        buf[pos++] = (char)((payload_len >> 40) & 0xFF);
        buf[pos++] = (char)((payload_len >> 32) & 0xFF);
        buf[pos++] = (char)((payload_len >> 24) & 0xFF);
        buf[pos++] = (char)((payload_len >> 16) & 0xFF);
        buf[pos++] = (char)((payload_len >> 8) & 0xFF);
        buf[pos++] = (char)(payload_len & 0xFF);
    }

    buf[pos++] = mask_key[0];
    buf[pos++] = mask_key[1];
    buf[pos++] = mask_key[2];
    buf[pos++] = mask_key[3];

    if (pos + payload_len > buf_size)
        return 0;

    for (size_t i = 0; i < payload_len; i++)
        buf[pos + i] = (char)((unsigned char)payload[i] ^ mask_key[i % 4]);
    pos += payload_len;

    return pos;
}

/* Apply XOR mask in-place (for server→client frames). */
void hu_ws_apply_mask(char *payload, size_t len, const unsigned char mask_key[4]) {
    for (size_t i = 0; i < len; i++)
        payload[i] = (char)((unsigned char)payload[i] ^ mask_key[i % 4]);
}

/* Parse WebSocket frame header. Returns 0 on success, non-zero on error. */
int hu_ws_parse_header(const char *bytes, size_t bytes_len, hu_ws_parsed_header_t *out) {
    if (bytes_len < 2 || !out)
        return -1;

    out->fin = (bytes[0] & 0x80) != 0;
    out->opcode = bytes[0] & 0x0F;
    out->masked = (bytes[1] & 0x80) != 0;
    uint64_t payload_len = bytes[1] & 0x7F;
    size_t hlen = 2;

    if (payload_len == 126) {
        if (bytes_len < 4)
            return -1;
        payload_len = ((uint64_t)(unsigned char)bytes[2] << 8) | (unsigned char)bytes[3];
        hlen = 4;
    } else if (payload_len == 127) {
        if (bytes_len < 10)
            return -1;
        payload_len = 0;
        for (int i = 2; i < 10; i++)
            payload_len = (payload_len << 8) | (unsigned char)bytes[i];
        hlen = 10;
    }

    if (out->masked)
        hlen += 4;
    if (payload_len > HU_WS_MAX_FRAME_PAYLOAD)
        return -1;

    out->payload_len = (unsigned long long)payload_len;
    out->header_bytes = hlen;
    return 0;
}

size_t hu_ws_build_upgrade_request(char *buf, size_t buf_cap, const char *host, const char *path,
                                   const char *ws_key, const char *extra_headers) {
    if (!host || !host[0] || !path || !path[0] || !ws_key || !ws_key[0])
        return 0;
    const char *xh = (extra_headers && extra_headers[0]) ? extra_headers : "";
    int n;
    if (!buf || buf_cap == 0) {
        n = snprintf(NULL, 0,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "%s"
                     "\r\n",
                     path, host, ws_key, xh);
    } else {
        n = snprintf(buf, buf_cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "%s"
                     "\r\n",
                     path, host, ws_key, xh);
    }
    if (n < 0)
        return 0;
    if (!buf || buf_cap == 0)
        return (size_t)n;
    if ((size_t)n >= buf_cap)
        return 0;
    return (size_t)n;
}

#if defined(HU_GATEWAY_POSIX)
/* Minimal SHA-1 for RFC 6455 Sec-WebSocket-Accept verification (client side). */

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} ws_sha1_ctx;

static uint32_t ws_sha1_rotl(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void ws_sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ws_sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ws_sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ws_sha1_rotl(b, 30);
        b = a;
        a = t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void ws_sha1_init(ws_sha1_ctx *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

static void ws_sha1_update(ws_sha1_ctx *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) {
            memcpy(ctx->buffer + idx, data, len);
            return;
        }
        memcpy(ctx->buffer + idx, data, fill);
        ws_sha1_transform(ctx->state, ctx->buffer);
        i = fill;
    }
    for (; i + 64 <= len; i += 64)
        ws_sha1_transform(ctx->state, data + i);
    if (i < len)
        memcpy(ctx->buffer, data + i, len - i);
}

static void ws_sha1_final(ws_sha1_ctx *ctx, uint8_t out[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        ws_sha1_transform(ctx->state, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    ws_sha1_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static size_t ws_b64_encode_bytes(const uint8_t *in, size_t in_len, char *out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((in_len + 2) / 3);
    if (olen + 1 > out_size)
        return 0;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len)
            v |= in[i + 2];
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = (i + 1 < in_len) ? tbl[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < in_len) ? tbl[v & 63] : '=';
    }
    out[j] = '\0';
    return j;
}

static bool ws_compute_sec_websocket_accept(const char *client_key, char *out, size_t out_size) {
    if (!client_key || out_size < 29)
        return false;
    size_t key_len = strlen(client_key);
    size_t magic_len = strlen(HU_WS_MAGIC);
    if (key_len + magic_len >= 128)
        return false;
    char cat[128];
    memcpy(cat, client_key, key_len);
    memcpy(cat + key_len, HU_WS_MAGIC, magic_len);

    ws_sha1_ctx ctx;
    ws_sha1_init(&ctx);
    ws_sha1_update(&ctx, (const uint8_t *)cat, key_len + magic_len);
    uint8_t hash[20];
    ws_sha1_final(&ctx, hash);

    return ws_b64_encode_bytes(hash, 20, out, out_size) > 0;
}

bool hu_ws_sec_websocket_accept_valid(const char *client_key_b64,
                                      const char *sec_websocket_accept) {
    if (!client_key_b64 || !sec_websocket_accept)
        return false;
    char expected[32];
    if (!ws_compute_sec_websocket_accept(client_key_b64, expected, sizeof(expected)))
        return false;
    return strcmp(expected, sec_websocket_accept) == 0;
}
#endif /* HU_GATEWAY_POSIX */

#if defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
/* Base64 encode 16 bytes into 24 chars. RFC 6455 accepts padding. */
static void ws_b64_encode_16(const unsigned char *in, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i;
    for (i = 0; i < 15; i += 3) {
        unsigned a = in[i], b = in[i + 1], c = in[i + 2];
        unsigned v = (a << 16) | (b << 8) | c;
        *out++ = tbl[(v >> 18) & 63];
        *out++ = tbl[(v >> 12) & 63];
        *out++ = tbl[(v >> 6) & 63];
        *out++ = tbl[v & 63];
    }
    /* Last byte: 16 % 3 = 1, so 2 chars + 2 padding */
    unsigned a = in[15];
    unsigned v = a << 16;
    *out++ = tbl[(v >> 18) & 63];
    *out++ = tbl[(v >> 12) & 63];
    *out++ = '=';
    *out++ = '=';
}

/* Parse ws:// or wss://host[:port][/path] into host, port, path.
 * Sets *use_tls to 1 for wss://, 0 for ws://. Returns 0 on success. */
static int ws_parse_url(const char *url, char *host, size_t host_size, uint16_t *port, char *path,
                        size_t path_size, int *use_tls) {
    int is_wss = 0;
    const char *p;
    if (strncmp(url, "wss://", 6) == 0) {
        is_wss = 1;
        p = url + 6;
        *port = 443;
    } else if (strncmp(url, "ws://", 5) == 0) {
        p = url + 5;
        *port = 80;
    } else {
        return -1;
    }
    if (use_tls)
        *use_tls = is_wss;

    const char *host_start = p;
    while (*p && *p != ':' && *p != '/')
        p++;
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= host_size)
        return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (*p == ':') {
        p++;
        unsigned pt = 0;
        while (*p >= '0' && *p <= '9') {
            pt = pt * 10 + (*p++ - '0');
            if (pt > 65535)
                return -1;
        }
        *port = (uint16_t)pt;
    }
    if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= path_size)
            return -1;
        memcpy(path, p, path_len + 1);
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}

static bool ws_http_extract_header(const char *block, size_t block_len, const char *name, char *out,
                                   size_t out_size) {
    const char *block_end = block + block_len;
    const char *p = block;
    size_t name_len = strlen(name);
    while (p < block_end) {
        if (name_len < (size_t)(block_end - p) && strncasecmp(p, name, name_len) == 0 &&
            p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < block_end && *v == ' ')
                v++;
            const char *end = v;
            while (end < block_end && *end && *end != '\r' && *end != '\n')
                end++;
            size_t vlen = (size_t)(end - v);
            if (vlen >= out_size)
                return false;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        while (p < block_end && *p && *p != '\n')
            p++;
        if (p < block_end && *p == '\n')
            p++;
    }
    return false;
}

static void ws_trim_ows_tail(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static bool ws_response_sec_websocket_accept_ok(const char *client_key_b64, const char *resp,
                                                size_t resp_len) {
    size_t hdr_end = (size_t)-1;
    for (size_t i = 0; i + 3 < resp_len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_end = i;
            break;
        }
    }
    if (hdr_end == (size_t)-1)
        return false;
    char accept[96];
    if (!ws_http_extract_header(resp, hdr_end, "Sec-WebSocket-Accept", accept, sizeof(accept)))
        return false;
    ws_trim_ows_tail(accept);
    return hu_ws_sec_websocket_accept_valid(client_key_b64, accept);
}
#endif

hu_error_t hu_ws_connect_with_headers(hu_allocator_t *alloc, const char *url,
                                      const char *extra_headers, hu_ws_client_t **out) {
    if (!alloc || !url || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    /* local_only: every remote websocket here is conversational (Gemini Live,
     * OpenAI Realtime, ws_streaming chat) unless it is an allowed voice
     * service (Cartesia streaming TTS). Checked before the transport. */
    hu_error_t lo = hu_local_only_check_ws(url);
    if (lo != HU_OK)
        return lo;

#if HU_IS_TEST
    (void)extra_headers;
    hu_ws_client_t *mock = (hu_ws_client_t *)alloc->alloc(alloc->ctx, sizeof(hu_ws_client_t));
    if (!mock)
        return HU_ERR_OUT_OF_MEMORY;
    memset(mock, 0, sizeof(*mock));
    mock->alloc = alloc;
    *out = mock;
    return HU_OK;
#elif defined(HU_GATEWAY_POSIX)
    int use_tls = 0;
#if !defined(HU_HAS_TLS)
    if (strncmp(url, "wss://", 6) == 0)
        return HU_ERR_NOT_SUPPORTED; /* TLS requires OpenSSL */
#endif

    char host[256], path[512];
    uint16_t port;
    if (ws_parse_url(url, host, sizeof(host), &port, path, sizeof(path), &use_tls) != 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (!use_tls) {
        hu_log_error("websocket", NULL,
                     "warning: connecting via ws:// (unencrypted). Prefer wss:// for production.");
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return HU_ERR_IO;

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        freeaddrinfo(res);
        return HU_ERR_IO;
    }
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        close(sockfd);
        freeaddrinfo(res);
        return HU_ERR_IO;
    }
    freeaddrinfo(res);

#ifdef HU_HAS_TLS
    SSL *ssl = NULL;
    SSL_CTX *ssl_ctx = NULL;
    if (use_tls) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            close(sockfd);
            return HU_ERR_IO;
        }
        SSL_CTX_set_default_verify_paths(ssl_ctx);
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);

        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            SSL_CTX_free(ssl_ctx);
            close(sockfd);
            return HU_ERR_IO;
        }
        SSL_set_fd(ssl, sockfd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
            close(sockfd);
            return HU_ERR_IO;
        }
    }
#endif

    /* Random 16-byte key for Sec-WebSocket-Key */
    unsigned char key_raw[16];
#if defined(__linux__) || defined(__APPLE__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t r = fread(key_raw, 1, 16, f);
        if (r < 16)
            memset(key_raw + r, 0, 16 - r);
        fclose(f);
    } else
#endif
    {
        /* Fallback: simple pseudo-random (not cryptographically secure but acceptable for nonce) */
        for (int i = 0; i < 16; i++)
            key_raw[i] = (unsigned char)((i * 31 + (unsigned long)url) & 0xFF);
    }
    char key_b64[32];
    ws_b64_encode_16(key_raw, key_b64);
    key_b64[24] = '\0';

    const char *xh = (extra_headers && extra_headers[0]) ? extra_headers : "";

    size_t need = hu_ws_build_upgrade_request(NULL, 0, host, path, key_b64, xh);
    if (need == 0) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t req_len = need + 1;
    char *req = (char *)alloc->alloc(alloc->ctx, req_len);
    if (!req) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (hu_ws_build_upgrade_request(req, req_len, host, path, key_b64, xh) == 0) {
        alloc->free(alloc->ctx, req, req_len);
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t sent = 0;
    size_t to_send = strlen(req);
    while (sent < to_send) {
#ifdef HU_HAS_TLS
        ssize_t n = ssl ? (ssize_t)SSL_write(ssl, req + sent, (int)(to_send - sent))
                        : send(sockfd, req + sent, to_send - sent, 0);
#else
        ssize_t n = send(sockfd, req + sent, to_send - sent, 0);
#endif
        if (n <= 0) {
            alloc->free(alloc->ctx, req, req_len);
#ifdef HU_HAS_TLS
            if (ssl) {
                SSL_free(ssl);
                SSL_CTX_free(ssl_ctx);
            }
#endif
            close(sockfd);
            return HU_ERR_IO;
        }
        sent += (size_t)n;
    }
    alloc->free(alloc->ctx, req, req_len);

    /* Read response (up to 4KB for headers) */
    char resp[4096];
    size_t resp_len = 0;
    while (resp_len < sizeof(resp) - 1) {
#ifdef HU_HAS_TLS
        ssize_t n =
            ssl ? (ssize_t)SSL_read(ssl, resp + resp_len, (int)(sizeof(resp) - 1 - resp_len))
                : recv(sockfd, resp + resp_len, sizeof(resp) - 1 - resp_len, 0);
#else
        ssize_t n = recv(sockfd, resp + resp_len, sizeof(resp) - 1 - resp_len, 0);
#endif
        if (n <= 0) {
#ifdef HU_HAS_TLS
            if (ssl) {
                SSL_free(ssl);
                SSL_CTX_free(ssl_ctx);
            }
#endif
            close(sockfd);
            return HU_ERR_IO;
        }
        resp_len += (size_t)n;
        resp[resp_len] = '\0';
        if (strstr(resp, "\r\n\r\n"))
            break;
    }
    if (strstr(resp, "\r\n\r\n") == NULL) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_IO;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_IO; /* Expected 101 Switching Protocols */
    }
    if (!strstr(resp, "Upgrade:") || !strstr(resp, "websocket")) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_IO;
    }
    if (!ws_response_sec_websocket_accept_ok(key_b64, resp, resp_len)) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_IO;
    }

    hu_ws_client_t *ws = (hu_ws_client_t *)alloc->alloc(alloc->ctx, sizeof(hu_ws_client_t));
    if (!ws) {
#ifdef HU_HAS_TLS
        if (ssl) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
#endif
        close(sockfd);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(ws, 0, sizeof(*ws));
    ws->alloc = alloc;
    ws->sockfd = sockfd;
#ifdef HU_HAS_TLS
    ws->ssl = ssl;
    ws->ssl_ctx = ssl_ctx;
#endif
    *out = ws;
    return HU_OK;
#else
    (void)alloc;
    (void)url;
    (void)extra_headers;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_ws_connect(hu_allocator_t *alloc, const char *url, hu_ws_client_t **out) {
    return hu_ws_connect_with_headers(alloc, url, NULL, out);
}

hu_error_t hu_ws_send(hu_ws_client_t *ws, const char *data, size_t data_len) {
    if (!ws || !data)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    (void)data_len;
    return HU_ERR_NOT_SUPPORTED;
#elif defined(HU_GATEWAY_POSIX)
    if (data_len > HU_WS_MAX_MSG)
        return HU_ERR_INVALID_ARGUMENT;
    if (ws->sockfd == HU_WS_INVALID_SOCK)
        return HU_ERR_IO;

    unsigned char mask_key[4];
#if defined(__linux__) || defined(__APPLE__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t r = fread(mask_key, 1, 4, f);
        if (r < 4)
            memset(mask_key + r, 0, 4 - r);
        fclose(f);
    } else
#endif
    {
        for (int i = 0; i < 4; i++)
            mask_key[i] = (unsigned char)((i * 17 + (unsigned long)data) & 0xFF);
    }

    bool use_heap = (data_len > 4096);
    char stack_buf[4096 + 14];
    char *buf = use_heap ? (char *)malloc(data_len + 14) : stack_buf;
    if (use_heap && !buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t buf_cap = use_heap ? (data_len + 14) : sizeof(stack_buf);
    size_t frame_len = hu_ws_build_frame(buf, buf_cap, HU_WS_OP_TEXT, data, data_len, mask_key);
    if (frame_len == 0) {
        if (use_heap)
            free(buf);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t sent = 0;
    hu_error_t werr = HU_OK;
    while (sent < frame_len) {
#ifdef HU_HAS_TLS
        ssize_t n = ws->ssl ? (ssize_t)SSL_write(ws->ssl, buf + sent, (int)(frame_len - sent))
                            : send(ws->sockfd, buf + sent, frame_len - sent, 0);
#else
        ssize_t n = send(ws->sockfd, buf + sent, frame_len - sent, 0);
#endif
        if (n <= 0) {
            werr = HU_ERR_IO;
            break;
        }
        sent += (size_t)n;
    }
    if (use_heap)
        free(buf);
    return werr;
#else
    (void)data_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_ws_recv(hu_ws_client_t *ws, hu_allocator_t *alloc, char **data_out,
                      size_t *data_len_out, int timeout_ms) {
    if (!ws || !alloc || !data_out || !data_len_out)
        return HU_ERR_INVALID_ARGUMENT;
    *data_out = NULL;
    *data_len_out = 0;

#if HU_IS_TEST
    (void)timeout_ms;
    return HU_ERR_NOT_SUPPORTED;
#elif defined(HU_GATEWAY_POSIX)
    if (ws->sockfd == HU_WS_INVALID_SOCK)
        return HU_ERR_IO;

    for (;;) {
        if (timeout_ms >= 0) {
            int ready = 0;
#ifdef HU_HAS_TLS
            if (ws->ssl && SSL_pending(ws->ssl) > 0)
                ready = 1;
#endif
            if (!ready) {
                struct pollfd pfd = {0};
                pfd.fd = ws->sockfd;
                pfd.events = POLLIN;
                int pr = poll(&pfd, 1, timeout_ms);
                if (pr == 0)
                    return HU_ERR_TIMEOUT;
                if (pr < 0)
                    return HU_ERR_IO;
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                    return HU_ERR_IO;
            }
        }

        char hdr_buf[14];
        size_t hdr_read = 0;
        while (hdr_read < 2) {
#ifdef HU_HAS_TLS
            ssize_t n = ws->ssl ? (ssize_t)SSL_read(ws->ssl, hdr_buf + hdr_read, 2 - hdr_read)
                                : recv(ws->sockfd, hdr_buf + hdr_read, 2 - hdr_read, 0);
#else
            ssize_t n = recv(ws->sockfd, hdr_buf + hdr_read, 2 - hdr_read, 0);
#endif
            if (n <= 0)
                return HU_ERR_IO;
            hdr_read += (size_t)n;
        }
        /* Compute full header size: 2 + (2 if len=126, 8 if len=127) + (4 if masked) */
        size_t need = 2;
        {
            uint8_t b1 = (uint8_t)hdr_buf[1];
            if ((b1 & 0x7F) == 126)
                need = 4;
            else if ((b1 & 0x7F) == 127)
                need = 10;
            if (b1 & 0x80)
                need += 4;
        }
        while (hdr_read < need) {
#ifdef HU_HAS_TLS
            ssize_t n = ws->ssl ? (ssize_t)SSL_read(ws->ssl, hdr_buf + hdr_read, need - hdr_read)
                                : recv(ws->sockfd, hdr_buf + hdr_read, need - hdr_read, 0);
#else
            ssize_t n = recv(ws->sockfd, hdr_buf + hdr_read, need - hdr_read, 0);
#endif
            if (n <= 0)
                return HU_ERR_IO;
            hdr_read += (size_t)n;
        }

        hu_ws_parsed_header_t hdr = {0};
        if (hu_ws_parse_header(hdr_buf, hdr_read, &hdr) != 0)
            return HU_ERR_PARSE;

        if (hdr.payload_len > HU_WS_MAX_MSG)
            return HU_ERR_IO;

        if ((hdr.opcode == HU_WS_OP_TEXT || hdr.opcode == HU_WS_OP_BINARY) && hdr.fin &&
            hdr.payload_len == 0) {
            if (ws->frag_buf) {
                alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                ws->frag_buf = NULL;
                ws->frag_len = 0;
                ws->frag_cap = 0;
            }
            char *empty = (char *)alloc->alloc(alloc->ctx, 1);
            if (!empty)
                return HU_ERR_OUT_OF_MEMORY;
            empty[0] = '\0';
            *data_out = empty;
            *data_len_out = 0;
            return HU_OK;
        }

        char *payload = NULL;
        if (hdr.payload_len > 0) {
            payload = (char *)alloc->alloc(alloc->ctx, hdr.payload_len + 1);
            if (!payload)
                return HU_ERR_OUT_OF_MEMORY;
            size_t pl_read = 0;
            while (pl_read < hdr.payload_len) {
#ifdef HU_HAS_TLS
                ssize_t n = ws->ssl
                                ? (ssize_t)SSL_read(ws->ssl, payload + pl_read,
                                                    (int)(hdr.payload_len - pl_read))
                                : recv(ws->sockfd, payload + pl_read, hdr.payload_len - pl_read, 0);
#else
                ssize_t n = recv(ws->sockfd, payload + pl_read, hdr.payload_len - pl_read, 0);
#endif
                if (n <= 0) {
                    alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                    return HU_ERR_IO;
                }
                pl_read += (size_t)n;
            }
            if (hdr.masked) {
                unsigned char mask[4];
                memcpy(mask, hdr_buf + hdr.header_bytes - 4, 4);
                hu_ws_apply_mask(payload, hdr.payload_len, mask);
            }
            payload[hdr.payload_len] = '\0';
        }

        switch (hdr.opcode) {
        case HU_WS_OP_PING:
            if (hdr.payload_len <= 125) {
                char pong_buf[128];
                unsigned char mask[4] = {0, 0, 0, 0};
                size_t pl = hu_ws_build_frame(pong_buf, sizeof(pong_buf), HU_WS_OP_PONG,
                                              payload ? payload : "", hdr.payload_len, mask);
                if (pl > 0) {
                    size_t sent = 0;
                    while (sent < pl) {
#ifdef HU_HAS_TLS
                        ssize_t nn =
                            ws->ssl ? (ssize_t)SSL_write(ws->ssl, pong_buf + sent, (int)(pl - sent))
                                    : send(ws->sockfd, pong_buf + sent, pl - sent, 0);
#else
                        ssize_t nn = send(ws->sockfd, pong_buf + sent, pl - sent, 0);
#endif
                        if (nn <= 0)
                            break;
                        sent += (size_t)nn;
                    }
                }
            }
            if (payload)
                alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        case HU_WS_OP_PONG:
            if (payload)
                alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        case HU_WS_OP_CLOSE:
            if (ws->frag_buf) {
                alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                ws->frag_buf = NULL;
                ws->frag_len = 0;
                ws->frag_cap = 0;
            }
            if (payload)
                alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            if (ws->sockfd != HU_WS_INVALID_SOCK) {
                char close_buf[16];
                unsigned char mask[4] = {0, 0, 0, 0};
                size_t n =
                    hu_ws_build_frame(close_buf, sizeof(close_buf), HU_WS_OP_CLOSE, "", 0, mask);
                if (n > 0) {
                    size_t sent = 0;
                    while (sent < n) {
#ifdef HU_HAS_TLS
                        ssize_t r =
                            ws->ssl ? (ssize_t)SSL_write(ws->ssl, close_buf + sent, (int)(n - sent))
                                    : send(ws->sockfd, close_buf + sent, n - sent, 0);
#else
                        ssize_t r = send(ws->sockfd, close_buf + sent, n - sent, 0);
#endif
                        if (r <= 0)
                            break;
                        sent += (size_t)r;
                    }
                }
#ifdef HU_HAS_TLS
                if (ws->ssl) {
                    SSL_shutdown(ws->ssl);
                    SSL_free(ws->ssl);
                    ws->ssl = NULL;
                }
                if (ws->ssl_ctx) {
                    SSL_CTX_free(ws->ssl_ctx);
                    ws->ssl_ctx = NULL;
                }
#endif
                close(ws->sockfd);
                ws->sockfd = HU_WS_INVALID_SOCK;
            }
            return HU_ERR_IO;
        case HU_WS_OP_TEXT:
        case HU_WS_OP_BINARY:
            if (hdr.fin) {
                if (ws->frag_buf) {
                    alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                    ws->frag_buf = NULL;
                    ws->frag_len = 0;
                    ws->frag_cap = 0;
                }
                *data_out = payload;
                *data_len_out = hdr.payload_len;
                if (!*data_out)
                    return HU_ERR_OUT_OF_MEMORY;
                return HU_OK;
            }
            /* First fragment of a multi-frame message */
            if (ws->frag_buf) {
                alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                ws->frag_buf = NULL;
                ws->frag_len = 0;
                ws->frag_cap = 0;
            }
            ws->frag_opcode = hdr.opcode;
            ws->frag_cap = hdr.payload_len > 0 ? hdr.payload_len + 1 : 64;
            ws->frag_buf = (char *)alloc->alloc(alloc->ctx, ws->frag_cap);
            if (!ws->frag_buf) {
                if (payload)
                    alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (payload && hdr.payload_len > 0)
                memcpy(ws->frag_buf, payload, hdr.payload_len);
            ws->frag_len = hdr.payload_len;
            if (payload)
                alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        case HU_WS_OP_CONTINUATION:
            if (!ws->frag_buf) {
                if (payload)
                    alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                continue;
            }
            if (hdr.payload_len > 0 && payload) {
                if (hdr.payload_len > SIZE_MAX - ws->frag_len) {
                    alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                    ws->frag_buf = NULL;
                    ws->frag_len = 0;
                    ws->frag_cap = 0;
                    alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                    return HU_ERR_IO;
                }
                size_t need_total = ws->frag_len + hdr.payload_len;
                if (need_total > HU_WS_MAX_MSG) {
                    alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                    ws->frag_buf = NULL;
                    ws->frag_len = 0;
                    ws->frag_cap = 0;
                    alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                    return HU_ERR_IO;
                }
                if (need_total + 1 > ws->frag_cap) {
                    size_t new_cap = ws->frag_cap * 2;
                    while (new_cap < need_total + 1)
                        new_cap *= 2;
                    char *nb =
                        (char *)alloc->realloc(alloc->ctx, ws->frag_buf, ws->frag_cap, new_cap);
                    if (!nb) {
                        alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
                        ws->frag_buf = NULL;
                        ws->frag_len = 0;
                        ws->frag_cap = 0;
                        alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    ws->frag_buf = nb;
                    ws->frag_cap = new_cap;
                }
                memcpy(ws->frag_buf + ws->frag_len, payload, hdr.payload_len);
                ws->frag_len += hdr.payload_len;
            }
            if (payload)
                alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            if (hdr.fin) {
                ws->frag_buf[ws->frag_len] = '\0';
                *data_out = ws->frag_buf;
                *data_len_out = ws->frag_len;
                ws->frag_buf = NULL;
                ws->frag_len = 0;
                ws->frag_cap = 0;
                return HU_OK;
            }
            continue;
        default:
            if (payload)
                alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        }
    }
#else
    (void)timeout_ms;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

void hu_ws_client_free(hu_ws_client_t *ws, hu_allocator_t *alloc) {
    if (!ws || !alloc)
        return;
    hu_ws_close(ws, alloc);
    alloc->free(alloc->ctx, ws, sizeof(struct hu_ws_client));
}

void hu_ws_close(hu_ws_client_t *ws, hu_allocator_t *alloc) {
    if (!ws)
        return;
#if defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
    if (ws->frag_buf && alloc) {
        alloc->free(alloc->ctx, ws->frag_buf, ws->frag_cap);
        ws->frag_buf = NULL;
        ws->frag_len = 0;
        ws->frag_cap = 0;
    }
    if (ws->sockfd != HU_WS_INVALID_SOCK) {
        char close_buf[16];
        unsigned char mask[4] = {0, 0, 0, 0};
        size_t n = hu_ws_build_frame(close_buf, sizeof(close_buf), HU_WS_OP_CLOSE, "", 0, mask);
        if (n > 0) {
            size_t sent = 0;
            while (sent < n) {
#ifdef HU_HAS_TLS
                ssize_t r = ws->ssl ? (ssize_t)SSL_write(ws->ssl, close_buf + sent, (int)(n - sent))
                                    : send(ws->sockfd, close_buf + sent, n - sent, 0);
#else
                ssize_t r = send(ws->sockfd, close_buf + sent, n - sent, 0);
#endif
                if (r <= 0)
                    break;
                sent += (size_t)r;
            }
        }
#ifdef HU_HAS_TLS
        if (ws->ssl) {
            SSL_shutdown(ws->ssl);
            SSL_free(ws->ssl);
            ws->ssl = NULL;
        }
        if (ws->ssl_ctx) {
            SSL_CTX_free(ws->ssl_ctx);
            ws->ssl_ctx = NULL;
        }
#endif
        close(ws->sockfd);
        ws->sockfd = HU_WS_INVALID_SOCK;
    }
#endif
    (void)alloc;
    memset(ws, 0, sizeof(*ws));
}
