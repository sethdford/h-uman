#include "seaclaw/gateway.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/crypto.h"
#include "seaclaw/gateway/control_protocol.h"
#include "seaclaw/gateway/event_bridge.h"
#include "seaclaw/gateway/ws_server.h"
#include "seaclaw/health.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SC_GATEWAY_POSIX
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SC_GATEWAY_DEFAULT_PORT    3000
#define SC_GATEWAY_POLL_TIMEOUT_MS 100

void sc_gateway_config_from_cfg(const sc_config_gateway_t *cfg_gw, sc_gateway_config_t *out) {
    if (!cfg_gw || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->host = cfg_gw->host && cfg_gw->host[0] ? cfg_gw->host : "0.0.0.0";
    out->port = cfg_gw->port > 0 ? cfg_gw->port : SC_GATEWAY_DEFAULT_PORT;
    out->max_body_size = SC_GATEWAY_MAX_BODY_SIZE;
    out->rate_limit_per_minute = cfg_gw->pair_rate_limit_per_minute > 0
                                     ? cfg_gw->pair_rate_limit_per_minute
                                     : SC_GATEWAY_RATE_LIMIT_PER_MIN;
    out->hmac_secret = cfg_gw->webhook_hmac_secret && cfg_gw->webhook_hmac_secret[0]
                           ? cfg_gw->webhook_hmac_secret
                           : NULL;
    out->hmac_secret_len = out->hmac_secret ? strlen(out->hmac_secret) : 0;
    if (cfg_gw->require_pairing && !out->hmac_secret) {
        const char *v = getenv("SEACLAW_WEBHOOK_HMAC_SECRET");
        if (v && v[0]) {
            out->hmac_secret = v;
            out->hmac_secret_len = strlen(v);
        }
    }
    out->test_mode = false;
    out->on_webhook = NULL;
    out->on_webhook_ctx = NULL;
    out->control_ui_dir = cfg_gw->control_ui_dir;
    out->cors_origins = (const char *const *)cfg_gw->cors_origins;
    out->cors_origins_len = cfg_gw->cors_origins_len;
    out->auth_token = NULL;
    out->control = NULL;
}

/* ── Rate limiting ──────────────────────────────────────────────────────── */

typedef struct rate_entry {
    char ip[64];
    uint32_t count;
    time_t window_start;
} rate_entry_t;

typedef struct sc_gateway_state {
    sc_allocator_t *alloc;
    sc_gateway_config_t config;
    int listen_fd;
    bool running;
    rate_entry_t rate_entries[256];
    size_t rate_count;
    sc_ws_server_t ws;
} sc_gateway_state_t;

static void trim_crlf(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static bool rate_limit_allow(sc_gateway_state_t *gw, const char *ip) {
    if (!gw)
        return true;
    time_t now = time(NULL);
    uint32_t limit = gw->config.rate_limit_per_minute ? gw->config.rate_limit_per_minute : 60;

    for (size_t i = 0; i < gw->rate_count; i++) {
        if (strcmp(gw->rate_entries[i].ip, ip) == 0) {
            if (now - gw->rate_entries[i].window_start >= 60) {
                gw->rate_entries[i].count = 0;
                gw->rate_entries[i].window_start = now;
            }
            gw->rate_entries[i].count++;
            return gw->rate_entries[i].count <= limit;
        }
    }
    if (gw->rate_count < 256) {
        strncpy(gw->rate_entries[gw->rate_count].ip, ip, 63);
        gw->rate_entries[gw->rate_count].ip[63] = '\0';
        gw->rate_entries[gw->rate_count].count = 1;
        gw->rate_entries[gw->rate_count].window_start = now;
        gw->rate_count++;
    }
    return true;
}

/* ── Path matching ──────────────────────────────────────────────────────── */

static bool path_is(const char *path, const char *base) {
    if (!path || !base)
        return false;
    size_t n = strlen(base);
    if (strncmp(path, base, n) != 0)
        return false;
    return path[n] == '\0' || (path[n] == '/' && path[n + 1] == '\0');
}

bool sc_gateway_path_is(const char *path, const char *base) {
    return path_is(path, base);
}

static bool is_webhook_path(const char *path) {
    return path_is(path, "/webhook") || strncmp(path, "/webhook/", 9) == 0 ||
           path_is(path, "/telegram") || path_is(path, "/slack/events") ||
           path_is(path, "/whatsapp") || path_is(path, "/line") || path_is(path, "/lark") ||
           path_is(path, "/discord");
}

static const char *webhook_path_to_channel(const char *path, char *buf, size_t buf_len) {
    if (!path || !buf || buf_len == 0)
        return "unknown";
    if (strncmp(path, "/webhook/", 9) == 0) {
        const char *rest = path + 9;
        const char *end = strchr(rest, '/');
        size_t n = end ? (size_t)(end - rest) : strlen(rest);
        if (n >= buf_len)
            n = buf_len - 1;
        memcpy(buf, rest, n);
        buf[n] = '\0';
        return buf;
    }
    if (path[0] == '/') {
        const char *rest = path + 1;
        const char *end = strchr(rest, '/');
        size_t n = end ? (size_t)(end - rest) : strlen(rest);
        if (n >= buf_len)
            n = buf_len - 1;
        memcpy(buf, rest, n);
        buf[n] = '\0';
        return buf;
    }
    return "unknown";
}

/* ── HMAC verification ──────────────────────────────────────────────────── */

static bool verify_hmac(const char *body, size_t body_len, const char *sig_header,
                        const char *secret, size_t secret_len) {
    if (!secret || secret_len == 0)
        return false;
    if (!sig_header)
        return false;
    uint8_t computed[32];
    sc_hmac_sha256((const uint8_t *)secret, secret_len, (const uint8_t *)body, body_len, computed);
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", computed[i]);
    hex[64] = '\0';
    size_t sig_len = strlen(sig_header);
    if (sig_len < 64)
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (unsigned char)sig_header[i] ^ (unsigned char)hex[i];
    return diff == 0;
}

/* ── HTTP response helpers ──────────────────────────────────────────────── */

static const char *const *s_cors_origins = NULL;
static size_t s_cors_origins_len = 0;

static bool is_allowed_origin(const char *origin) {
    if (!origin || !origin[0])
        return true;
    if (strstr(origin, "://localhost") != NULL || strstr(origin, "://127.0.0.1") != NULL ||
        strstr(origin, "://[::1]") != NULL)
        return true;
    for (size_t i = 0; i < s_cors_origins_len; i++) {
        if (s_cors_origins[i] && strcmp(origin, s_cors_origins[i]) == 0)
            return true;
    }
    return false;
}

static const char *s_request_origin = NULL;

static void send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
}

static void send_response(int fd, int status, const char *content_type, const char *body,
                          size_t body_len) {
    const char *status_str = "200 OK";
    if (status == 404)
        status_str = "404 Not Found";
    else if (status == 429)
        status_str = "429 Too Many Requests";
    else if (status == 413)
        status_str = "413 Payload Too Large";
    else if (status == 401)
        status_str = "401 Unauthorized";
    else if (status == 304)
        status_str = "304 Not Modified";

    char hdr[512];
    const char *cors_origin = "";
    char cors_line[256] = "";
    if (s_request_origin && is_allowed_origin(s_request_origin)) {
        cors_origin = s_request_origin;
        snprintf(cors_line, sizeof(cors_line), "Access-Control-Allow-Origin: %s\r\n", cors_origin);
    }
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %zu\r\n"
                     "%s"
                     "\r\n",
                     status_str, content_type, body_len, cors_line);
    send_all(fd, hdr, (size_t)n);
    if (body && body_len > 0)
        send_all(fd, body, body_len);
}

static void send_json(int fd, int status, const char *body) {
    send_response(fd, status, "application/json", body, body ? strlen(body) : 0);
}

/* ── Static file serving ────────────────────────────────────────────────── */

static const char *mime_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (strcmp(dot, ".html") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0)
        return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".mjs") == 0)
        return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0)
        return "application/json; charset=utf-8";
    if (strcmp(dot, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(dot, ".woff2") == 0)
        return "font/woff2";
    if (strcmp(dot, ".woff") == 0)
        return "font/woff";
    return "application/octet-stream";
}

#ifdef SC_GATEWAY_POSIX
static bool serve_static_file(int fd, const char *base_dir, const char *url_path) {
    if (!base_dir || !url_path)
        return false;
    if (strstr(url_path, "..") || strstr(url_path, "%2e%2e") || strstr(url_path, "%2E%2E") ||
        strstr(url_path, "%2e%2E") || strstr(url_path, "%2E%2e") || strstr(url_path, "%00"))
        return false;

    char filepath[1024];
    const char *rel = url_path;
    if (rel[0] == '/')
        rel++;
    if (rel[0] == '\0')
        rel = "index.html";

    snprintf(filepath, sizeof(filepath), "%s/%s", base_dir, rel);

    int file_fd = open(filepath, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (file_fd < 0 || fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (file_fd >= 0)
            close(file_fd);
        /* SPA fallback: serve index.html for unmatched routes */
        snprintf(filepath, sizeof(filepath), "%s/index.html", base_dir);
        file_fd = open(filepath, O_RDONLY | O_NOFOLLOW);
        if (file_fd < 0 || fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            if (file_fd >= 0)
                close(file_fd);
            return false;
        }
    }

    if (st.st_size > 10 * 1024 * 1024) {
        close(file_fd);
        return false;
    }

    FILE *f = fdopen(file_fd, "rb");
    if (!f) {
        close(file_fd);
        return false;
    }

    size_t fsize = (size_t)st.st_size;
    char *buf = (char *)malloc(fsize);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t rd = fread(buf, 1, fsize, f);
    fclose(f);

    const char *mime = mime_for_ext(filepath);
    send_response(fd, 200, mime, buf, rd);
    free(buf);
    return true;
}
#endif

/* ── HTTP request handling ──────────────────────────────────────────────── */

#ifdef SC_GATEWAY_POSIX
static void handle_http_request(sc_gateway_state_t *gw, int fd, const char *method,
                                const char *path, const char *body, size_t body_len,
                                const char *client_ip, const char *sig_header) {

    fprintf(stderr, "[gateway] %s %s %s body=%zu\n", method ? method : "?", path ? path : "/",
            client_ip ? client_ip : "unknown", body_len);

    if (!rate_limit_allow(gw, client_ip)) {
        send_json(fd, 429, "{\"error\":\"rate limited\"}");
        return;
    }

    if (path_is(path, "/ready") || path_is(path, "/readyz")) {
        sc_allocator_t alloc = sc_system_allocator();
        sc_readiness_result_t r = sc_health_check_readiness(&alloc);
        char *json = sc_sprintf(&alloc, "{\"status\":\"%s\",\"checks\":[]}",
                                r.status == SC_READINESS_READY ? "ready" : "not_ready");
        send_json(fd, 200, json);
        if (json)
            alloc.free(alloc.ctx, json, strlen(json) + 1);
        if (r.checks)
            alloc.free(alloc.ctx, (void *)r.checks, r.check_count * sizeof(sc_component_check_t));
        return;
    }

    if (path_is(path, "/health") || path_is(path, "/healthz")) {
        send_json(fd, 200, "{\"status\":\"ok\"}");
        return;
    }

    /* API endpoint: capabilities snapshot */
    if (path_is(path, "/api/status")) {
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"websocket\":%s,\"connections\":%zu}",
                 "true", gw->ws.conn_count);
        send_json(fd, 200, resp);
        return;
    }

    if (is_webhook_path(path)) {
        if (gw && gw->config.hmac_secret && gw->config.hmac_secret_len > 0) {
            if (!verify_hmac(body, body_len, sig_header, gw->config.hmac_secret,
                             gw->config.hmac_secret_len)) {
                send_json(fd, 401, "{\"error\":\"invalid signature\"}");
                return;
            }
        }
        char ch_buf[32];
        const char *channel = webhook_path_to_channel(path, ch_buf, sizeof(ch_buf));
        if (gw->config.test_mode)
            (void)fprintf(stderr, "[gateway] webhook received channel=%s\n", channel);
        if (gw && gw->config.on_webhook)
            gw->config.on_webhook(channel, body, body_len, gw->config.on_webhook_ctx);
        send_json(fd, 200, "{\"received\":true}");
        return;
    }

    /* API namespace always returns 404 for unknown paths */
    if (strncmp(path, "/api/", 5) == 0) {
        send_json(fd, 404, "{\"error\":\"not found\"}");
        return;
    }

    /* Static file serving (Control UI) — SPA fallback for non-API routes */
    if (gw->config.control_ui_dir) {
        if (serve_static_file(fd, gw->config.control_ui_dir, path))
            return;
    }

    send_json(fd, 404, "{\"error\":\"not found\"}");
}
#endif

/* ── Main gateway run loop (poll-based) ─────────────────────────────────── */

sc_error_t sc_gateway_run(sc_allocator_t *alloc, const char *host, uint16_t port,
                          const sc_gateway_config_t *config) {
    (void)host;
    (void)port;
    if (!alloc)
        return SC_ERR_INVALID_ARGUMENT;

    sc_gateway_config_t cfg = {0};
    if (config)
        memcpy(&cfg, config, sizeof(cfg));
    if (!cfg.host)
        cfg.host = "0.0.0.0";
    if (cfg.port == 0)
        cfg.port = SC_GATEWAY_DEFAULT_PORT;
    if (cfg.max_body_size == 0)
        cfg.max_body_size = SC_GATEWAY_MAX_BODY_SIZE;
    if (cfg.rate_limit_per_minute == 0)
        cfg.rate_limit_per_minute = SC_GATEWAY_RATE_LIMIT_PER_MIN;

    if (cfg.test_mode) {
        sc_health_mark_ok("gateway");
        return SC_OK;
    }

    s_cors_origins = cfg.cors_origins;
    s_cors_origins_len = cfg.cors_origins_len;

#ifdef SC_GATEWAY_POSIX
    sc_gateway_state_t *gw = NULL;
    int fd = -1;
    char *body_buf = NULL;
    sc_error_t err = SC_OK;
    sc_control_protocol_t *ctrl = NULL;
    sc_control_protocol_t proto_local;
    sc_event_bridge_t event_bridge;
    bool bridge_active = false;

    memset(&event_bridge, 0, sizeof(event_bridge));

    gw = (sc_gateway_state_t *)alloc->alloc(alloc->ctx, sizeof(sc_gateway_state_t));
    if (!gw) {
        err = SC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(gw, 0, sizeof(*gw));
    gw->alloc = alloc;
    gw->config = cfg;

    ctrl = cfg.control ? cfg.control : &proto_local;
    sc_control_protocol_init(ctrl, alloc, &gw->ws);
    sc_ws_server_init(&gw->ws, alloc, sc_control_on_message, sc_control_on_close, ctrl);
    gw->ws.auth_token = cfg.auth_token;

    if (cfg.app_ctx) {
        sc_control_set_app_ctx(ctrl, cfg.app_ctx);
        if (cfg.app_ctx->bus) {
            sc_event_bridge_init(&event_bridge, ctrl, cfg.app_ctx->bus);
            bridge_active = true;
        }
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        sc_health_mark_error("gateway", "socket failed");
        err = SC_ERR_IO;
        goto cleanup;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    inet_pton(AF_INET, cfg.host, &addr.sin_addr);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        sc_health_mark_error("gateway", "bind failed");
        err = SC_ERR_IO;
        goto cleanup;
    }

    if (listen(fd, 64) < 0) {
        sc_health_mark_error("gateway", "listen failed");
        err = SC_ERR_IO;
        goto cleanup;
    }

    /* Set listen socket non-blocking for poll */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    gw->listen_fd = fd;
    gw->running = true;
    sc_health_mark_ok("gateway");

    body_buf = (char *)alloc->alloc(alloc->ctx, cfg.max_body_size + 1);
    if (!body_buf) {
        err = SC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    fprintf(stderr, "[gateway] listening on %s:%u (ws: enabled, ui: %s)\n", cfg.host,
            (unsigned)cfg.port, cfg.control_ui_dir ? cfg.control_ui_dir : "disabled");

    /* Poll-based event loop: listen socket + WebSocket connections */
    while (gw->running) {
        struct pollfd fds[1 + SC_WS_SERVER_MAX_CONNS];
        int nfds = 0;

        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        int listen_idx = nfds++;

        int ws_indices[SC_WS_SERVER_MAX_CONNS];
        int ws_count = 0;
        for (int i = 0; i < SC_WS_SERVER_MAX_CONNS; i++) {
            if (gw->ws.conns[i].active) {
                ws_indices[ws_count] = i;
                fds[nfds].fd = gw->ws.conns[i].fd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                nfds++;
                ws_count++;
            }
        }

        int ready = poll(fds, (nfds_t)nfds, SC_GATEWAY_POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;

        /* Handle WebSocket connections first */
        for (int w = 0; w < ws_count; w++) {
            int poll_idx = listen_idx + 1 + w;
            if (fds[poll_idx].revents & (POLLIN | POLLERR | POLLHUP)) {
                int ci = ws_indices[w];
                sc_ws_server_read_and_process(&gw->ws, &gw->ws.conns[ci]);
            }
        }

        /* Handle new connections on listen socket */
        if (fds[listen_idx].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client = accept(fd, (struct sockaddr *)&client_addr, &client_len);
            if (client < 0)
                continue;

            char client_ip[64];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

            /* Ensure client socket is blocking (macOS accept inherits O_NONBLOCK) */
            {
                int flags = fcntl(client, F_GETFL, 0);
                if (flags >= 0 && (flags & O_NONBLOCK))
                    fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
            }

            char req[4096];
            size_t total = 0;
            {
                struct timeval tv = {5, 0};
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            }
            while (total < sizeof(req) - 1) {
                ssize_t r = recv(client, req + total, sizeof(req) - 1 - total, 0);
                if (r <= 0)
                    break;
                total += (size_t)r;
                req[total] = '\0';
                if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
                    break;
            }
            ssize_t n = (ssize_t)total;
            if (n <= 0) {
                close(client);
                continue;
            }

            /* Check for WebSocket upgrade */
            if (sc_ws_server_is_upgrade(req, (size_t)n)) {
                sc_ws_conn_t *conn = NULL;
                sc_error_t ws_err = sc_ws_server_upgrade(&gw->ws, client, req, (size_t)n, &conn);
                if (ws_err != SC_OK) {
                    send_json(client, 429, "{\"error\":\"too many connections\"}");
                    close(client);
                } else {
                    fprintf(stderr, "[gateway] ws connected id=%llu ip=%s\n",
                            (unsigned long long)conn->id, client_ip);
                }
                continue;
            }

            /* Regular HTTP request */
            char *line = strtok(req, "\n");
            char method[16] = {0}, path[256] = {0};
            if (line)
                sscanf(line, "%15s %255s", method, path);

            size_t body_len = 0;
            char *sig_header = NULL;
            bool rejected = false;
            while ((line = strtok(NULL, "\n")) != NULL) {
                trim_crlf(line);
                if (line[0] == '\0')
                    break;
                if (strncasecmp(line, "Content-Length:", 15) == 0) {
                    char *cl_end;
                    long v = strtol(line + 15, &cl_end, 10);
                    if (v < 0 || cl_end == line + 15) {
                        send_json(client, 400, "{\"error\":\"bad request\"}");
                        close(client);
                        rejected = true;
                        break;
                    }
                    body_len = (size_t)v;
                    if (body_len > cfg.max_body_size) {
                        send_json(client, 413, "{\"error\":\"body too large\"}");
                        close(client);
                        rejected = true;
                        body_len = 0;
                        break;
                    }
                }
                if (strncasecmp(line, "X-Signature:", 12) == 0) {
                    char *v = line + 12;
                    while (*v == ' ')
                        v++;
                    sig_header = v;
                }
                if (strncasecmp(line, "Origin:", 7) == 0) {
                    char *v = line + 7;
                    while (*v == ' ')
                        v++;
                    s_request_origin = v;
                }
            }

            if (rejected) {
                s_request_origin = NULL;
            }

            if (!rejected) {
                if (body_len > 0 && body_len <= cfg.max_body_size) {
                    size_t got = 0;
                    while (got < body_len) {
                        ssize_t r = recv(client, body_buf + got, body_len - got, 0);
                        if (r <= 0)
                            break;
                        got += (size_t)r;
                    }
                    body_buf[body_len] = '\0';
                }

                handle_http_request(gw, client, method, path, body_buf, body_len, client_ip,
                                    sig_header);
                s_request_origin = NULL;
                {
                    struct linger sl = {1, 5};
                    setsockopt(client, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
                    shutdown(client, SHUT_WR);
                    char drain[256];
                    struct timeval tv = {1, 0};
                    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    while (recv(client, drain, sizeof(drain), 0) > 0) {}
                }
                close(client);
            }
        }
    }

cleanup:
    if (bridge_active)
        sc_event_bridge_deinit(&event_bridge);
    sc_ws_server_deinit(&gw->ws);
    if (ctrl == &proto_local)
        sc_control_protocol_deinit(ctrl);
    if (body_buf)
        alloc->free(alloc->ctx, body_buf, cfg.max_body_size + 1);
    if (fd >= 0)
        close(fd);
    if (gw)
        alloc->free(alloc->ctx, gw, sizeof(*gw));
    return err;
#endif

#ifndef SC_GATEWAY_POSIX
    (void)host;
    (void)port;
    (void)config;
#endif
    return SC_OK;
}
