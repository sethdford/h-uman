#include "seaclaw/websocket/websocket.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(SC_GATEWAY_POSIX) && !SC_IS_TEST
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

#ifdef SC_HAS_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#define SC_WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define SC_WS_MAX_FRAME_PAYLOAD (4 * 1024 * 1024)
#define SC_WS_MAX_MSG (64 * 1024)

/* RFC 6455 opcodes */
#define SC_WS_OP_CONTINUATION 0
#define SC_WS_OP_TEXT         1
#define SC_WS_OP_BINARY       2
#define SC_WS_OP_CLOSE        8
#define SC_WS_OP_PING         9
#define SC_WS_OP_PONG         10

#define SC_WS_INVALID_SOCK (-1)

struct sc_ws_client {
    sc_allocator_t *alloc;
#if defined(SC_GATEWAY_POSIX) && !SC_IS_TEST
    int sockfd;
#ifdef SC_HAS_TLS
    SSL *ssl;
    SSL_CTX *ssl_ctx;
#endif
#endif
};

/* Build a masked WebSocket frame into buf. Returns bytes written. */
size_t sc_ws_build_frame(char *buf, size_t buf_size,
    unsigned opcode,
    const char *payload, size_t payload_len,
    const unsigned char mask_key[4])
{
    if (buf_size < 6 || payload_len > SC_WS_MAX_FRAME_PAYLOAD)
        return 0;

    size_t pos = 0;
    buf[pos++] = (char)(0x80 | (opcode & 0x0F));  /* FIN=1, opcode */

    if (payload_len <= 125) {
        buf[pos++] = (char)(0x80 | (payload_len & 0x7F));  /* MASK=1, len */
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
void sc_ws_apply_mask(char *payload, size_t len, const unsigned char mask_key[4])
{
    for (size_t i = 0; i < len; i++)
        payload[i] = (char)((unsigned char)payload[i] ^ mask_key[i % 4]);
}

/* Parse WebSocket frame header. Returns 0 on success, non-zero on error. */
int sc_ws_parse_header(const char *bytes, size_t bytes_len, sc_ws_parsed_header_t *out)
{
    if (bytes_len < 2 || !out) return -1;

    out->fin = (bytes[0] & 0x80) != 0;
    out->opcode = bytes[0] & 0x0F;
    out->masked = (bytes[1] & 0x80) != 0;
    uint64_t payload_len = bytes[1] & 0x7F;
    size_t hlen = 2;

    if (payload_len == 126) {
        if (bytes_len < 4) return -1;
        payload_len = ((uint64_t)(unsigned char)bytes[2] << 8) | (unsigned char)bytes[3];
        hlen = 4;
    } else if (payload_len == 127) {
        if (bytes_len < 10) return -1;
        payload_len = 0;
        for (int i = 2; i < 10; i++)
            payload_len = (payload_len << 8) | (unsigned char)bytes[i];
        hlen = 10;
    }

    if (out->masked) hlen += 4;
    if (payload_len > SC_WS_MAX_FRAME_PAYLOAD) return -1;

    out->payload_len = (unsigned long long)payload_len;
    out->header_bytes = hlen;
    return 0;
}

#if defined(SC_GATEWAY_POSIX) && !SC_IS_TEST
/* Base64 encode 16 bytes into 24 chars. RFC 6455 accepts padding. */
static void ws_b64_encode_16(const unsigned char *in, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
static int ws_parse_url(const char *url, char *host, size_t host_size,
    uint16_t *port, char *path, size_t path_size, int *use_tls)
{
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
    if (use_tls) *use_tls = is_wss;

    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= host_size) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (*p == ':') {
        p++;
        unsigned pt = 0;
        while (*p >= '0' && *p <= '9') { pt = pt * 10 + (*p++ - '0'); if (pt > 65535) return -1; }
        *port = (uint16_t)pt;
    }
    if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= path_size) return -1;
        memcpy(path, p, path_len + 1);
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return 0;
}
#endif

sc_error_t sc_ws_connect(sc_allocator_t *alloc,
    const char *url,
    sc_ws_client_t **out)
{
    if (!alloc || !url || !out) return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;

#if SC_IS_TEST
    (void)alloc;
    return SC_ERR_NOT_SUPPORTED;
#elif defined(SC_GATEWAY_POSIX)
    int use_tls = 0;
#if !defined(SC_HAS_TLS)
    if (strncmp(url, "wss://", 6) == 0)
        return SC_ERR_NOT_SUPPORTED; /* TLS requires OpenSSL */
#endif

    char host[256], path[512];
    uint16_t port;
    if (ws_parse_url(url, host, sizeof(host), &port, path, sizeof(path), &use_tls) != 0)
        return SC_ERR_INVALID_ARGUMENT;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return SC_ERR_IO;

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        freeaddrinfo(res);
        return SC_ERR_IO;
    }
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        close(sockfd);
        freeaddrinfo(res);
        return SC_ERR_IO;
    }
    freeaddrinfo(res);

#ifdef SC_HAS_TLS
    SSL *ssl = NULL;
    SSL_CTX *ssl_ctx = NULL;
    if (use_tls) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            close(sockfd);
            return SC_ERR_IO;
        }
        SSL_CTX_set_default_verify_paths(ssl_ctx);
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);

        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            SSL_CTX_free(ssl_ctx);
            close(sockfd);
            return SC_ERR_IO;
        }
        SSL_set_fd(ssl, sockfd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
            close(sockfd);
            return SC_ERR_IO;
        }
    }
#endif

    /* Random 16-byte key for Sec-WebSocket-Key */
    unsigned char key_raw[16];
#if defined(__linux__) || defined(__APPLE__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        (void)fread(key_raw, 1, 16, f);
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

    size_t req_len = 0;
    req_len += (size_t)snprintf(NULL, 0,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, key_b64) + 1;
    char *req = (char *)alloc->alloc(alloc->ctx, req_len);
    if (!req) {
#ifdef SC_HAS_TLS
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
        close(sockfd);
        return SC_ERR_OUT_OF_MEMORY;
    }
    (void)snprintf(req, req_len,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, key_b64);
    size_t sent = 0;
    size_t to_send = strlen(req);
    while (sent < to_send) {
#ifdef SC_HAS_TLS
        ssize_t n = ssl ? (ssize_t)SSL_write(ssl, req + sent, (int)(to_send - sent)) : send(sockfd, req + sent, to_send - sent, 0);
#else
        ssize_t n = send(sockfd, req + sent, to_send - sent, 0);
#endif
        if (n <= 0) {
            alloc->free(alloc->ctx, req, req_len);
#ifdef SC_HAS_TLS
            if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
            close(sockfd);
            return SC_ERR_IO;
        }
        sent += (size_t)n;
    }
    alloc->free(alloc->ctx, req, req_len);

    /* Read response (up to 4KB for headers) */
    char resp[4096];
    size_t resp_len = 0;
    while (resp_len < sizeof(resp) - 1) {
#ifdef SC_HAS_TLS
        ssize_t n = ssl ? (ssize_t)SSL_read(ssl, resp + resp_len, (int)(sizeof(resp) - 1 - resp_len)) : recv(sockfd, resp + resp_len, sizeof(resp) - 1 - resp_len, 0);
#else
        ssize_t n = recv(sockfd, resp + resp_len, sizeof(resp) - 1 - resp_len, 0);
#endif
        if (n <= 0) {
#ifdef SC_HAS_TLS
            if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
            close(sockfd);
            return SC_ERR_IO;
        }
        resp_len += (size_t)n;
        resp[resp_len] = '\0';
        if (strstr(resp, "\r\n\r\n"))
            break;
    }
    if (strstr(resp, "\r\n\r\n") == NULL) {
#ifdef SC_HAS_TLS
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
        close(sockfd);
        return SC_ERR_IO;
    }
    if (!strstr(resp, "101")) {
#ifdef SC_HAS_TLS
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
        close(sockfd);
        return SC_ERR_IO; /* Expected 101 Switching Protocols */
    }
    if (!strstr(resp, "Upgrade:") || !strstr(resp, "websocket")) {
#ifdef SC_HAS_TLS
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
        close(sockfd);
        return SC_ERR_IO;
    }

    sc_ws_client_t *ws = (sc_ws_client_t *)alloc->alloc(alloc->ctx, sizeof(sc_ws_client_t));
    if (!ws) {
#ifdef SC_HAS_TLS
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
#endif
        close(sockfd);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(ws, 0, sizeof(*ws));
    ws->alloc = alloc;
    ws->sockfd = sockfd;
#ifdef SC_HAS_TLS
    ws->ssl = ssl;
    ws->ssl_ctx = ssl_ctx;
#endif
    *out = ws;
    return SC_OK;
#else
    (void)alloc;
    (void)url;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

sc_error_t sc_ws_send(sc_ws_client_t *ws,
    const char *data,
    size_t data_len)
{
    if (!ws || !data) return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    (void)data_len;
    return SC_ERR_NOT_SUPPORTED;
#elif defined(SC_GATEWAY_POSIX)
    if (data_len > SC_WS_MAX_MSG) return SC_ERR_INVALID_ARGUMENT;
    if (ws->sockfd == SC_WS_INVALID_SOCK) return SC_ERR_IO;

    unsigned char mask_key[4];
#if defined(__linux__) || defined(__APPLE__)
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        (void)fread(mask_key, 1, 4, f);
        fclose(f);
    } else
#endif
    {
        for (int i = 0; i < 4; i++)
            mask_key[i] = (unsigned char)((i * 17 + (unsigned long)data) & 0xFF);
    }

    char buf[SC_WS_MAX_MSG + 14]; /* max header 14 bytes for len 64K */
    size_t frame_len = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_TEXT,
        data, data_len, mask_key);
    if (frame_len == 0) return SC_ERR_INVALID_ARGUMENT;

    size_t sent = 0;
    while (sent < frame_len) {
#ifdef SC_HAS_TLS
        ssize_t n = ws->ssl ? (ssize_t)SSL_write(ws->ssl, buf + sent, (int)(frame_len - sent)) : send(ws->sockfd, buf + sent, frame_len - sent, 0);
#else
        ssize_t n = send(ws->sockfd, buf + sent, frame_len - sent, 0);
#endif
        if (n <= 0) return SC_ERR_IO;
        sent += (size_t)n;
    }
    return SC_OK;
#else
    (void)data_len;
    return SC_ERR_NOT_SUPPORTED;
#endif
}

sc_error_t sc_ws_recv(sc_ws_client_t *ws,
    sc_allocator_t *alloc,
    char **data_out,
    size_t *data_len_out)
{
    if (!ws || !alloc || !data_out || !data_len_out) return SC_ERR_INVALID_ARGUMENT;
    *data_out = NULL;
    *data_len_out = 0;

#if SC_IS_TEST
    return SC_ERR_NOT_SUPPORTED;
#elif defined(SC_GATEWAY_POSIX)
    if (ws->sockfd == SC_WS_INVALID_SOCK) return SC_ERR_IO;

    for (;;) {
        char hdr_buf[14];
        size_t hdr_read = 0;
        while (hdr_read < 2) {
#ifdef SC_HAS_TLS
            ssize_t n = ws->ssl ? (ssize_t)SSL_read(ws->ssl, hdr_buf + hdr_read, 2 - hdr_read) : recv(ws->sockfd, hdr_buf + hdr_read, 2 - hdr_read, 0);
#else
            ssize_t n = recv(ws->sockfd, hdr_buf + hdr_read, 2 - hdr_read, 0);
#endif
            if (n <= 0) return SC_ERR_IO;
            hdr_read += (size_t)n;
        }
        /* Compute full header size: 2 + (2 if len=126, 8 if len=127) + (4 if masked) */
        size_t need = 2;
        {
            uint8_t b1 = (uint8_t)hdr_buf[1];
            if ((b1 & 0x7F) == 126) need = 4;
            else if ((b1 & 0x7F) == 127) need = 10;
            if (b1 & 0x80) need += 4;
        }
        while (hdr_read < need) {
#ifdef SC_HAS_TLS
            ssize_t n = ws->ssl ? (ssize_t)SSL_read(ws->ssl, hdr_buf + hdr_read, need - hdr_read) : recv(ws->sockfd, hdr_buf + hdr_read, need - hdr_read, 0);
#else
            ssize_t n = recv(ws->sockfd, hdr_buf + hdr_read, need - hdr_read, 0);
#endif
            if (n <= 0) return SC_ERR_IO;
            hdr_read += (size_t)n;
        }

        sc_ws_parsed_header_t hdr = {0};
        if (sc_ws_parse_header(hdr_buf, hdr_read, &hdr) != 0)
            return SC_ERR_PARSE;

        if (hdr.payload_len > SC_WS_MAX_MSG)
            return SC_ERR_IO;

        char *payload = NULL;
        if (hdr.payload_len > 0) {
            payload = (char *)alloc->alloc(alloc->ctx, hdr.payload_len + 1);
            if (!payload) return SC_ERR_OUT_OF_MEMORY;
            size_t pl_read = 0;
            while (pl_read < hdr.payload_len) {
#ifdef SC_HAS_TLS
                ssize_t n = ws->ssl ? (ssize_t)SSL_read(ws->ssl, payload + pl_read, (int)(hdr.payload_len - pl_read)) : recv(ws->sockfd, payload + pl_read, hdr.payload_len - pl_read, 0);
#else
                ssize_t n = recv(ws->sockfd, payload + pl_read, hdr.payload_len - pl_read, 0);
#endif
                if (n <= 0) {
                    alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
                    return SC_ERR_IO;
                }
                pl_read += (size_t)n;
            }
            if (hdr.masked) {
                unsigned char mask[4];
                memcpy(mask, hdr_buf + hdr.header_bytes - 4, 4);
                sc_ws_apply_mask(payload, hdr.payload_len, mask);
            }
            payload[hdr.payload_len] = '\0';
        }

        switch (hdr.opcode) {
        case SC_WS_OP_PING:
            if (hdr.payload_len <= 125) {
                char pong_buf[128];
                unsigned char mask[4] = {0, 0, 0, 0};
                size_t pl = sc_ws_build_frame(pong_buf, sizeof(pong_buf),
                    SC_WS_OP_PONG, payload ? payload : "", hdr.payload_len, mask);
                if (pl > 0) {
                    size_t sent = 0;
                    while (sent < pl) {
#ifdef SC_HAS_TLS
                        ssize_t nn = ws->ssl ? (ssize_t)SSL_write(ws->ssl, pong_buf + sent, (int)(pl - sent)) : send(ws->sockfd, pong_buf + sent, pl - sent, 0);
#else
                        ssize_t nn = send(ws->sockfd, pong_buf + sent, pl - sent, 0);
#endif
                        if (nn <= 0) break;
                        sent += (size_t)nn;
                    }
                }
            }
            if (payload) alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        case SC_WS_OP_PONG:
        case SC_WS_OP_CONTINUATION:
            if (payload) alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        case SC_WS_OP_CLOSE:
            if (payload) alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            ws->sockfd = SC_WS_INVALID_SOCK;
            return SC_ERR_IO; /* Connection closed by server */
        case SC_WS_OP_TEXT:
        case SC_WS_OP_BINARY:
            *data_out = payload ? payload : (char *)alloc->alloc(alloc->ctx, 1);
            *data_len_out = hdr.payload_len;
            if (!*data_out && hdr.payload_len > 0) return SC_ERR_OUT_OF_MEMORY;
            if (hdr.payload_len == 0 && *data_out) {
                (*data_out)[0] = '\0';
            }
            return SC_OK;
        default:
            if (payload) alloc->free(alloc->ctx, payload, hdr.payload_len + 1);
            continue;
        }
    }
#else
    return SC_ERR_NOT_SUPPORTED;
#endif
}

void sc_ws_close(sc_ws_client_t *ws, sc_allocator_t *alloc)
{
    if (!ws) return;
#if defined(SC_GATEWAY_POSIX) && !SC_IS_TEST
    if (ws->sockfd != SC_WS_INVALID_SOCK) {
        char close_buf[16];
        unsigned char mask[4] = {0, 0, 0, 0};
        size_t n = sc_ws_build_frame(close_buf, sizeof(close_buf),
            SC_WS_OP_CLOSE, "", 0, mask);
        if (n > 0) {
            size_t sent = 0;
            while (sent < n) {
#ifdef SC_HAS_TLS
                ssize_t r = ws->ssl ? (ssize_t)SSL_write(ws->ssl, close_buf + sent, (int)(n - sent)) : send(ws->sockfd, close_buf + sent, n - sent, 0);
#else
                ssize_t r = send(ws->sockfd, close_buf + sent, n - sent, 0);
#endif
                if (r <= 0) break;
                sent += (size_t)r;
            }
        }
#ifdef SC_HAS_TLS
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
        ws->sockfd = SC_WS_INVALID_SOCK;
    }
#endif
    (void)alloc;
    memset(ws, 0, sizeof(*ws));
}
