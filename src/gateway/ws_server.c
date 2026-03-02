#include "seaclaw/gateway/ws_server.h"
#include <stdint.h>
#include "seaclaw/websocket/websocket.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>

#ifdef SC_GATEWAY_POSIX
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#endif

/* macOS doesn't have MSG_NOSIGNAL; use SO_NOSIGPIPE on the socket instead */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ── Minimal SHA-1 for WebSocket accept key (RFC 6455 requires it) ───────── */

typedef struct sha1_ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} sha1_ctx_t;

static uint32_t sha1_rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;             k = 0xCA62C1D6; }
        uint32_t t = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rotl(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) { memcpy(ctx->buffer + idx, data, len); return; }
        memcpy(ctx->buffer + idx, data, fill);
        sha1_transform(ctx->state, ctx->buffer);
        i = fill;
    }
    for (; i + 64 <= len; i += 64)
        sha1_transform(ctx->state, data + i);
    if (i < len)
        memcpy(ctx->buffer, data + i, len - i);
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t out[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx->buffer + idx, 0, 64 - idx);
        sha1_transform(ctx->state, ctx->buffer);
        idx = 0;
    }
    memset(ctx->buffer + idx, 0, 56 - idx);
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/* ── Base64 encode ──────────────────────────────────────────────────────── */

static size_t b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = 4 * ((in_len + 2) / 3);
    if (olen + 1 > out_size) return 0;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= in[i + 2];
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = (i + 1 < in_len) ? tbl[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < in_len) ? tbl[v & 63] : '=';
    }
    out[j] = '\0';
    return j;
}

/* ── Compute WebSocket accept key: SHA-1(client_key + magic) → base64 ── */

static bool compute_accept_key(const char *client_key, char *out, size_t out_size) {
    if (!client_key || out_size < 29) return false;
    size_t key_len = strlen(client_key);
    size_t magic_len = strlen(WS_MAGIC);
    size_t cat_len = key_len + magic_len;
    char cat[128];
    if (cat_len >= sizeof(cat)) return false;
    memcpy(cat, client_key, key_len);
    memcpy(cat + key_len, WS_MAGIC, magic_len);
    cat[cat_len] = '\0';

    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)cat, cat_len);
    uint8_t hash[20];
    sha1_final(&ctx, hash);

    return b64_encode(hash, 20, out, out_size) > 0;
}

/* ── Extract header value from HTTP request ─────────────────────────────── */

static bool extract_header(const char *req, size_t req_len,
    const char *name, char *out, size_t out_size) {
    (void)req_len;
    const char *p = req;
    size_t name_len = strlen(name);
    while (*p) {
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (*v == ' ') v++;
            const char *end = v;
            while (*end && *end != '\r' && *end != '\n') end++;
            size_t vlen = (size_t)(end - v);
            if (vlen >= out_size) return false;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return false;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void sc_ws_server_init(sc_ws_server_t *srv, sc_allocator_t *alloc,
    sc_ws_server_on_message_fn on_message,
    sc_ws_server_on_close_fn on_close,
    void *cb_ctx) {
    if (!srv) return;
    memset(srv, 0, sizeof(*srv));
    srv->alloc = alloc;
    srv->on_message = on_message;
    srv->on_close = on_close;
    srv->cb_ctx = cb_ctx;
    srv->next_id = 1;
    for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++)
        srv->conns[i].fd = -1;
}

void sc_ws_server_deinit(sc_ws_server_t *srv) {
    if (!srv) return;
    for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++) {
        if (srv->conns[i].active)
            sc_ws_server_close_conn(srv, &srv->conns[i]);
    }
}

bool sc_ws_server_is_upgrade(const char *req, size_t req_len) {
    if (!req || req_len < 20) return false;
    char upgrade[32] = {0};
    char connection[64] = {0};
    if (!extract_header(req, req_len, "Upgrade", upgrade, sizeof(upgrade)))
        return false;
    if (!extract_header(req, req_len, "Connection", connection, sizeof(connection)))
        return false;
    bool has_ws = (strcasecmp(upgrade, "websocket") == 0);
    bool has_upgrade = false;
    {
        const char *p = connection;
        while (*p) {
            while (*p == ' ' || *p == ',') p++;
            if (strncasecmp(p, "Upgrade", 7) == 0) {
                char c = p[7];
                if (c == '\0' || c == ',' || c == ' ' || c == '\r' || c == '\n') {
                    has_upgrade = true;
                    break;
                }
            }
            while (*p && *p != ',') p++;
        }
    }
    return has_ws && has_upgrade;
}

sc_error_t sc_ws_server_upgrade(sc_ws_server_t *srv, int fd,
    const char *req, size_t req_len,
    sc_ws_conn_t **out) {
    if (!srv || fd < 0 || !req || !out) return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;

#ifndef SC_GATEWAY_POSIX
    (void)req_len;
    return SC_ERR_NOT_SUPPORTED;
#else
    sc_ws_conn_t *slot = NULL;
    for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++) {
        if (!srv->conns[i].active) { slot = &srv->conns[i]; break; }
    }
    if (!slot) return SC_ERR_ALREADY_EXISTS;

    /* Authenticate WebSocket upgrade if auth_token is configured */
    if (srv->auth_token && srv->auth_token[0]) {
        char auth_hdr[256] = {0};
        if (!extract_header(req, req_len, "Authorization", auth_hdr, sizeof(auth_hdr))) {
            const char *r401 = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            send(fd, r401, strlen(r401), 0);
            return SC_ERR_PERMISSION_DENIED;
        }
        if (strncmp(auth_hdr, "Bearer ", 7) != 0) {
            const char *r401 = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            send(fd, r401, strlen(r401), 0);
            return SC_ERR_PERMISSION_DENIED;
        }
        const char *tok = auth_hdr + 7;
        size_t tok_len = strlen(tok);
        size_t exp_len = strlen(srv->auth_token);
        unsigned char d = (tok_len != exp_len) ? 1 : 0;
        size_t cmp_len = tok_len < exp_len ? tok_len : exp_len;
        for (size_t i = 0; i < cmp_len; i++)
            d |= (unsigned char)tok[i] ^ (unsigned char)srv->auth_token[i];
        if (d != 0) {
            const char *r401 = "HTTP/1.1 401 Unauthorized\r\n\r\n";
            send(fd, r401, strlen(r401), 0);
            return SC_ERR_PERMISSION_DENIED;
        }
    }

    /* Validate Origin header — only allow localhost origins */
    char origin[256] = {0};
    if (extract_header(req, req_len, "Origin", origin, sizeof(origin))) {
        if (!strstr(origin, "://localhost") &&
            !strstr(origin, "://127.0.0.1") &&
            !strstr(origin, "://[::1]")) {
            const char *r403 = "HTTP/1.1 403 Forbidden\r\n\r\n";
            send(fd, r403, strlen(r403), 0);
            return SC_ERR_PERMISSION_DENIED;
        }
    }

    char ws_key[128] = {0};
    if (!extract_header(req, req_len, "Sec-WebSocket-Key", ws_key, sizeof(ws_key)))
        return SC_ERR_INVALID_ARGUMENT;

    char accept[64] = {0};
    if (!compute_accept_key(ws_key, accept, sizeof(accept)))
        return SC_ERR_INVALID_ARGUMENT;

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept);

    size_t sent = 0;
    while (sent < (size_t)n) {
        ssize_t w = send(fd, resp + sent, (size_t)n - sent, 0);
        if (w <= 0) return SC_ERR_IO;
        sent += (size_t)w;
    }

    memset(slot, 0, sizeof(*slot));
    slot->fd = fd;
    slot->active = true;
    slot->authenticated = false;
    slot->id = srv->next_id++;
    slot->recv_len = 0;
    srv->conn_count++;

    /* Set non-blocking for poll-based reads */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    *out = slot;
    return SC_OK;
#endif
}

/* Build an unmasked server→client text frame */
static size_t build_server_frame(char *buf, size_t buf_size,
    const char *payload, size_t payload_len) {
    if (buf_size < 2 || payload_len > SC_WS_SERVER_MAX_MSG) return 0;
    size_t pos = 0;
    buf[pos++] = (char)(0x80 | SC_WS_OP_TEXT);
    if (payload_len <= 125) {
        buf[pos++] = (char)(payload_len & 0x7F);
    } else if (payload_len <= 65535) {
        buf[pos++] = (char)126;
        buf[pos++] = (char)((payload_len >> 8) & 0xFF);
        buf[pos++] = (char)(payload_len & 0xFF);
    } else {
        buf[pos++] = (char)127;
        for (int i = 7; i >= 0; i--)
            buf[pos++] = (char)((payload_len >> (i * 8)) & 0xFF);
    }
    if (pos + payload_len > buf_size) return 0;
    memcpy(buf + pos, payload, payload_len);
    return pos + payload_len;
}

sc_error_t sc_ws_server_send(sc_ws_conn_t *conn,
    const char *data, size_t data_len) {
    if (!conn || !conn->active || conn->fd < 0 || !data)
        return SC_ERR_INVALID_ARGUMENT;
#ifndef SC_GATEWAY_POSIX
    (void)data_len;
    return SC_ERR_NOT_SUPPORTED;
#else
    char buf[SC_WS_SERVER_MAX_MSG + 14];
    size_t frame_len = build_server_frame(buf, sizeof(buf), data, data_len);
    if (frame_len == 0) return SC_ERR_INVALID_ARGUMENT;

    size_t sent = 0;
    while (sent < frame_len) {
        ssize_t w = send(conn->fd, buf + sent, frame_len - sent, MSG_NOSIGNAL);
        if (w <= 0) return SC_ERR_IO;
        sent += (size_t)w;
    }
    return SC_OK;
#endif
}

void sc_ws_server_broadcast(sc_ws_server_t *srv,
    const char *data, size_t data_len) {
    if (!srv || !data) return;
    for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++) {
        if (srv->conns[i].active) {
            sc_error_t err = sc_ws_server_send(&srv->conns[i], data, data_len);
            if (err != SC_OK)
                sc_ws_server_close_conn(srv, &srv->conns[i]);
        }
    }
}

void sc_ws_server_close_conn(sc_ws_server_t *srv, sc_ws_conn_t *conn) {
    if (!conn || !conn->active) return;
#ifdef SC_GATEWAY_POSIX
    /* Send close frame */
    char close_frame[4];
    close_frame[0] = (char)(0x80 | SC_WS_OP_CLOSE);
    close_frame[1] = 0;
    (void)send(conn->fd, close_frame, 2, MSG_NOSIGNAL);
    close(conn->fd);
#endif
    if (srv && srv->on_close)
        srv->on_close(conn, srv->cb_ctx);
    conn->fd = -1;
    conn->active = false;
    conn->authenticated = false;
    conn->recv_len = 0;
    if (srv && srv->conn_count > 0)
        srv->conn_count--;
}

sc_error_t sc_ws_server_read_and_process(sc_ws_server_t *srv,
    sc_ws_conn_t *conn) {
    if (!srv || !conn || !conn->active) return SC_ERR_INVALID_ARGUMENT;
#ifndef SC_GATEWAY_POSIX
    return SC_ERR_NOT_SUPPORTED;
#else
    size_t space = SC_WS_SERVER_RECV_BUF - conn->recv_len;
    if (space == 0) {
        /* Buffer full with no complete frame — likely a malformed or oversized
           message.  Close the connection instead of silently discarding data. */
        sc_ws_server_close_conn(srv, conn);
        return SC_ERR_IO;
    }
    ssize_t n = recv(conn->fd, conn->recv_buf + conn->recv_len, space, 0);
    if (n <= 0) {
        sc_ws_server_close_conn(srv, conn);
        return SC_ERR_IO;
    }
    conn->recv_len += (size_t)n;
    return sc_ws_server_process(srv, conn);
#endif
}

sc_error_t sc_ws_server_process(sc_ws_server_t *srv, sc_ws_conn_t *conn) {
    if (!srv || !conn || !conn->active) return SC_ERR_INVALID_ARGUMENT;

    while (conn->recv_len >= 2) {
        sc_ws_parsed_header_t hdr = {0};
        if (sc_ws_parse_header(conn->recv_buf, conn->recv_len, &hdr) != 0)
            break;

        size_t total = hdr.header_bytes + (size_t)hdr.payload_len;
        if (total > conn->recv_len) break;

        char *payload = conn->recv_buf + hdr.header_bytes;
        size_t plen = (size_t)hdr.payload_len;

        if (hdr.masked && plen > 0) {
            unsigned char mask[4];
            memcpy(mask, conn->recv_buf + hdr.header_bytes - 4, 4);
            sc_ws_apply_mask(payload, plen, mask);
        }

        switch (hdr.opcode) {
        case SC_WS_OP_TEXT:
        case SC_WS_OP_BINARY:
            if (srv->on_message)
                srv->on_message(conn, payload, plen, srv->cb_ctx);
            break;
        case SC_WS_OP_PING: {
#ifdef SC_GATEWAY_POSIX
            /* RFC 6455 s5.5: control frame payload max is 125 bytes.
               Always reply with PONG echoing the payload. */
            if (plen > 125) plen = 125;
            char pong[2 + 125];
            pong[0] = (char)(0x80 | SC_WS_OP_PONG);
            pong[1] = (char)(plen & 0x7F);
            if (plen > 0) memcpy(pong + 2, payload, plen);
            (void)send(conn->fd, pong, 2 + plen, MSG_NOSIGNAL);
#endif
            break;
        }
        case SC_WS_OP_CLOSE:
            sc_ws_server_close_conn(srv, conn);
            return SC_ERR_IO;
        default:
            break;
        }

        size_t remaining = conn->recv_len - total;
        if (remaining > 0)
            memmove(conn->recv_buf, conn->recv_buf + total, remaining);
        conn->recv_len = remaining;
    }
    return SC_OK;
}
