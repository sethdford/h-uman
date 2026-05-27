/* h-uman doctor WebSocket consumer — see header for spec.
 *
 * T1 (this file): pure helpers only — format_event_line and
 * event_matches_filter. hu_doctor_ws_watch returns
 * HU_ERR_NOT_SUPPORTED for now; T2-T6 land the socket loop.
 *
 * Spec: docs/plans/2026-05-27-doctor-ws-consumer/
 */

#include "human/doctor/ws_consumer.h"
#include "human/core/string.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if !defined(HU_IS_TEST) && (defined(__unix__) || defined(__APPLE__))
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#define WS_CONSUMER_HAS_NET 1
#else
#define WS_CONSUMER_HAS_NET 0
#endif

/* ── RFC 6455 WebSocket handshake helpers ──────────────────────────────────
 * These helpers are intentional duplicates of static functions in
 * src/gateway/ws_server.c (sha1_*, b64_encode, compute_accept_key).
 * Keeping them sibling-local rather than extracting into a shared module
 * because:
 *  - ws_server.c is hot production code; refactoring its static helpers is
 *    a separate concern that should land via its own commit + tests
 *  - The crypto is RFC-fixed (SHA-1 / base64 / GUID 258EAFA5-...); they
 *    won't drift unless the WebSocket spec changes
 *  - T7 of the spec (docs/plans/2026-05-27-doctor-ws-consumer/tasks.md)
 *    is a contract test that runs both sides against the same fixture,
 *    so any silent divergence is caught before it ships
 *
 * If/when more callers need these helpers, lift them to
 * include/human/gateway/ws_handshake.h. See FOLLOWUP comment at file end.
 * ────────────────────────────────────────────────────────────────────────── */

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct ws_sha1_ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} ws_sha1_ctx_t;

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

static void ws_sha1_init(ws_sha1_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

static void ws_sha1_update(ws_sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
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

static void ws_sha1_final(ws_sha1_ctx_t *ctx, uint8_t out[20]) {
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

static size_t ws_b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size) {
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

/* Compute the Sec-WebSocket-Accept value from the client_key, per RFC 6455.
 * Returns true on success and writes a NUL-terminated base64 string into
 * `out` (must be at least 29 bytes — 28 + NUL for a SHA1 → base64).
 *
 * EXPORTED INTERNALLY (file-static) — public-API-style wrappers exposed
 * below for the test suite. */
static bool ws_compute_accept_key(const char *client_key, char *out, size_t out_size) {
    if (!client_key || out_size < 29)
        return false;
    size_t key_len = strlen(client_key);
    size_t magic_len = strlen(WS_MAGIC);
    size_t cat_len = key_len + magic_len;
    char cat[128];
    if (cat_len >= sizeof(cat))
        return false;
    memcpy(cat, client_key, key_len);
    memcpy(cat + key_len, WS_MAGIC, magic_len);
    cat[cat_len] = '\0';

    ws_sha1_ctx_t ctx;
    ws_sha1_init(&ctx);
    ws_sha1_update(&ctx, (const uint8_t *)cat, cat_len);
    uint8_t hash[20];
    ws_sha1_final(&ctx, hash);
    return ws_b64_encode(hash, 20, out, out_size) > 0;
}

/* Public, internal-test surface for ws_compute_accept_key. */
bool hu_doctor_ws__compute_accept_key(const char *client_key, char *out, size_t out_size) {
    return ws_compute_accept_key(client_key, out, out_size);
}

/* ── Random client_key generator ──────────────────────────────────────────
 * 16 random bytes from /dev/urandom, base64-encoded (yields a 24-char
 * string ending with "==" for 16-byte input).
 *
 * Under HU_IS_TEST, returns a fixed deterministic key so tests can pin
 * the exact handshake bytes.
 */
bool hu_doctor_ws__generate_client_key(char *out, size_t out_size) {
    if (out_size < 25)
        return false;
    uint8_t key[16];
#if HU_IS_TEST
    /* Deterministic key for tests — 16 bytes = "test-key-1234567" */
    static const uint8_t fixed[16] = {'t', 'e', 's', 't', '-', 'k', 'e', 'y',
                                      '-', '1', '2', '3', '4', '5', '6', '7'};
    memcpy(key, fixed, 16);
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t r = read(fd, key, sizeof(key));
    close(fd);
    if (r != (ssize_t)sizeof(key))
        return false;
#endif
    return ws_b64_encode(key, sizeof(key), out, out_size) > 0;
}

/* Format the HTTP upgrade request bytes (no socket I/O — pure).
 * Returns number of bytes written, or 0 on overflow/error. */
size_t hu_doctor_ws__format_upgrade_request(char *buf, size_t buf_size, const char *host,
                                            uint16_t port, const char *path,
                                            const char *client_key) {
    if (!buf || !host || !path || !client_key)
        return 0;
    int n = snprintf(buf, buf_size,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     path, host, (unsigned)port, client_key);
    if (n <= 0 || (size_t)n >= buf_size)
        return 0;
    return (size_t)n;
}

/* Parse the server's handshake response. Returns true iff:
 *  1. Status line is "HTTP/1.1 101 Switching Protocols"
 *  2. Sec-WebSocket-Accept header matches `expected_accept`
 *
 * Pure (no I/O) — caller passes the response bytes already read. */
bool hu_doctor_ws__verify_handshake_response(const char *resp, size_t resp_len,
                                             const char *expected_accept) {
    if (!resp || !expected_accept)
        return false;
    /* Status line: must start with "HTTP/1.1 101" */
    if (resp_len < 12 || strncmp(resp, "HTTP/1.1 101", 12) != 0)
        return false;
    /* Find Sec-WebSocket-Accept header (case-insensitive). */
    static const char *needle = "Sec-WebSocket-Accept:";
    size_t needle_len = strlen(needle);
    const char *p = resp;
    const char *end = resp + resp_len;
    while (p + needle_len <= end) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            const char *v = p + needle_len;
            while (v < end && (*v == ' ' || *v == '\t'))
                v++;
            const char *vend = v;
            while (vend < end && *vend != '\r' && *vend != '\n')
                vend++;
            size_t vlen = (size_t)(vend - v);
            size_t exp_len = strlen(expected_accept);
            return vlen == exp_len && memcmp(v, expected_accept, exp_len) == 0;
        }
        /* advance to next line */
        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;
    }
    return false;
}

hu_doctor_ws_config_t hu_doctor_ws_config_default(void) {
    hu_doctor_ws_config_t c = {0};
    c.host = "127.0.0.1";
    c.port = 3006;
    c.path = "/ws";
    c.event_filter = NULL;
    c.log_path = NULL;
    c.max_reconnect_attempts = 3;
    c.quiet_stdout = false;
    return c;
}

/* Walk a comma-separated filter, calling `match` for each token (with
 * surrounding whitespace trimmed). Returns immediately when match returns
 * true. Returns false if no token matches (or filter_csv is empty).
 *
 * Pure scan helper used by event_matches_filter; kept private. */
static bool filter_csv_any_token(const char *filter_csv,
                                 bool (*match)(const char *token, size_t len, void *ctx),
                                 void *ctx) {
    if (!filter_csv || !*filter_csv)
        return false;
    const char *p = filter_csv;
    while (*p) {
        /* skip leading whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        /* trim trailing whitespace */
        while (end > start && isspace((unsigned char)*(end - 1)))
            end--;
        if (end > start) {
            if (match(start, (size_t)(end - start), ctx))
                return true;
        }
        if (*p == ',')
            p++;
    }
    return false;
}

struct match_eq_ctx {
    const char *name;
    size_t name_len;
};

static bool match_eq(const char *token, size_t len, void *ctx_in) {
    struct match_eq_ctx *ctx = (struct match_eq_ctx *)ctx_in;
    return len == ctx->name_len && strncmp(token, ctx->name, len) == 0;
}

bool hu_doctor_ws_event_matches_filter(const char *event_name, const char *filter_csv) {
    if (!event_name)
        return false;
    /* NULL or empty filter = match all */
    if (!filter_csv || !*filter_csv)
        return true;
    struct match_eq_ctx ctx = {.name = event_name, .name_len = strlen(event_name)};
    return filter_csv_any_token(filter_csv, match_eq, &ctx);
}

/* Trim and shorten a JSON payload to a one-line summary for the unknown-
 * event-type fallback. Drops newlines, collapses runs of whitespace,
 * truncates to `max_len` characters with a trailing "..." if cut. Caller
 * owns the returned buffer (via alloc->free with len+1). */
static char *payload_compact_oneline(hu_allocator_t *alloc, const char *payload_json,
                                     size_t max_len) {
    if (!payload_json)
        return hu_strdup(alloc, "");
    size_t src_len = strlen(payload_json);
    size_t cap = src_len + 4; /* room for "..." + NUL */
    char *out = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out)
        return NULL;
    size_t w = 0;
    bool last_was_space = false;
    for (size_t i = 0; i < src_len && w + 4 < cap; i++) {
        char c = payload_json[i];
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ') {
            if (last_was_space)
                continue;
            last_was_space = true;
        } else {
            last_was_space = false;
        }
        if (w >= max_len) {
            out[w++] = '.';
            out[w++] = '.';
            out[w++] = '.';
            break;
        }
        out[w++] = c;
    }
    out[w] = '\0';
    return out;
}

/* For known event types, emit a more useful summary. For unknown types,
 * fall back to the compacted JSON. */
static char *summarize_payload(hu_allocator_t *alloc, const char *event_name,
                               const char *payload_json) {
    /* For now, defer specialized formatters to a follow-up commit and
     * just return a compact-JSON fallback. T1 ships the structure;
     * T3+ will plug in per-event formatters once the parser is in. */
    (void)event_name;
    return payload_compact_oneline(alloc, payload_json, 80);
}

char *hu_doctor_ws_format_event_line(hu_allocator_t *alloc, const char *event_name,
                                     const char *payload_json, uint64_t seq, int64_t now_epoch) {
    if (!alloc || !event_name)
        return NULL;
    time_t t = (now_epoch > 0) ? (time_t)now_epoch : time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);

    char *summary = summarize_payload(alloc, event_name, payload_json);
    if (!summary)
        return NULL;
    size_t summary_len = strlen(summary);

    /* "[HH:MM:SS] <name> seq=<seq> <summary>" — generous buffer. */
    size_t cap = summary_len + strlen(event_name) + 64;
    char *out = (char *)alloc->alloc(alloc->ctx, cap);
    if (!out) {
        alloc->free(alloc->ctx, summary, summary_len + 1);
        return NULL;
    }
    int n = snprintf(out, cap, "[%02d:%02d:%02d] %s seq=%llu %s", lt.tm_hour, lt.tm_min, lt.tm_sec,
                     event_name, (unsigned long long)seq, summary);
    alloc->free(alloc->ctx, summary, summary_len + 1);
    if (n <= 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, out, cap);
        return NULL;
    }
    return out;
}

hu_error_t hu_doctor_ws_watch(hu_allocator_t *alloc, const hu_doctor_ws_config_t *cfg) {
    (void)alloc;
    (void)cfg;
    /* T2-T6 — see docs/plans/2026-05-27-doctor-ws-consumer/tasks.md */
    return HU_ERR_NOT_SUPPORTED;
}
