#include "seaclaw/gateway.h"
#include "seaclaw/config.h"
#include "seaclaw/health.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef SC_GATEWAY_POSIX
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#endif

#define SC_GATEWAY_DEFAULT_PORT 8080

void sc_gateway_config_from_cfg(const sc_config_gateway_t *cfg_gw,
                                sc_gateway_config_t *out) {
    if (!cfg_gw || !out) return;
    memset(out, 0, sizeof(*out));
    out->host = cfg_gw->host && cfg_gw->host[0] ? cfg_gw->host : "0.0.0.0";
    out->port = cfg_gw->port > 0 ? cfg_gw->port : SC_GATEWAY_DEFAULT_PORT;
    out->max_body_size = SC_GATEWAY_MAX_BODY_SIZE;
    out->rate_limit_per_minute = cfg_gw->pair_rate_limit_per_minute > 0 ?
        cfg_gw->pair_rate_limit_per_minute : SC_GATEWAY_RATE_LIMIT_PER_MIN;
    out->hmac_secret = cfg_gw->webhook_hmac_secret && cfg_gw->webhook_hmac_secret[0] ?
        cfg_gw->webhook_hmac_secret : NULL;
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
}

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
} sc_gateway_state_t;

static void trim_crlf(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

static bool rate_limit_allow(sc_gateway_state_t *gw, const char *ip) {
    if (!gw) return true;
    time_t now = time(NULL);
    uint32_t limit = gw->config.rate_limit_per_minute ?
        gw->config.rate_limit_per_minute : 60;

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

static bool path_is(const char *path, const char *base) {
    size_t n = strlen(base);
    if (strncmp(path, base, n) != 0) return false;
    return path[n] == '\0' || (path[n] == '/' && path[n + 1] == '\0');
}

static bool is_webhook_path(const char *path) {
    return path_is(path, "/webhook") ||
        strncmp(path, "/webhook/", 9) == 0 ||
        path_is(path, "/telegram") ||
        path_is(path, "/slack/events") ||
        path_is(path, "/whatsapp") ||
        path_is(path, "/line") ||
        path_is(path, "/lark") ||
        path_is(path, "/discord");
}

static const char *webhook_path_to_channel(const char *path, char *buf, size_t buf_len) {
    if (!path || !buf || buf_len == 0) return "unknown";
    if (strncmp(path, "/webhook/", 9) == 0) {
        const char *rest = path + 9;
        const char *end = strchr(rest, '/');
        size_t n = end ? (size_t)(end - rest) : strlen(rest);
        if (n >= buf_len) n = buf_len - 1;
        memcpy(buf, rest, n);
        buf[n] = '\0';
        return buf;
    }
    if (path[0] == '/') {
        const char *rest = path + 1;
        const char *end = strchr(rest, '/');
        size_t n = end ? (size_t)(end - rest) : strlen(rest);
        if (n >= buf_len) n = buf_len - 1;
        memcpy(buf, rest, n);
        buf[n] = '\0';
        return buf;
    }
    return "unknown";
}

static bool verify_hmac(const char *body, size_t body_len,
    const char *sig_header, const char *secret, size_t secret_len) {
    if (!secret || secret_len == 0) return true;
    if (!sig_header) return false;

    uint8_t computed[32];
    sc_hmac_sha256((const uint8_t *)secret, secret_len,
        (const uint8_t *)body, body_len, computed);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", computed[i]);
    hex[64] = '\0';

    return strncmp(sig_header, hex, 64) == 0;
}

static void send_response(int fd, int status, const char *body) {
    const char *status_str = "200 OK";
    if (status == 404) status_str = "404 Not Found";
    else if (status == 429) status_str = "429 Too Many Requests";
    else if (status == 413) status_str = "413 Payload Too Large";
    else if (status == 401) status_str = "401 Unauthorized";

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        status_str, body ? strlen(body) : 0, body ? body : "");
    send(fd, buf, (size_t)n, 0);
}

#ifdef SC_GATEWAY_POSIX
static void handle_request(sc_gateway_state_t *gw, int fd, const char *method,
    const char *path, const char *body, size_t body_len, const char *client_ip,
    const char *sig_header) {
    (void)method;

    if (!rate_limit_allow(gw, client_ip)) {
        send_response(fd, 429, "{\"error\":\"rate limited\"}");
        return;
    }

    if (path_is(path, "/ready")) {
        sc_allocator_t alloc = sc_system_allocator();
        sc_readiness_result_t r = sc_health_check_readiness(&alloc);
        char *json = sc_sprintf(&alloc, "{\"status\":\"%s\",\"checks\":[]}",
            r.status == SC_READINESS_READY ? "ready" : "not_ready");
        send_response(fd, 200, json);
        if (json) alloc.free(alloc.ctx, json, strlen(json) + 1);
        if (r.checks) alloc.free(alloc.ctx, (void *)r.checks,
            r.check_count * sizeof(sc_component_check_t));
        return;
    }

    if (path_is(path, "/health")) {
        send_response(fd, 200, "{\"status\":\"ok\"}");
        return;
    }

    if (is_webhook_path(path)) {
        if (gw && gw->config.hmac_secret && gw->config.hmac_secret_len > 0) {
            if (!verify_hmac(body, body_len, sig_header,
                    gw->config.hmac_secret, gw->config.hmac_secret_len)) {
                send_response(fd, 401, "{\"error\":\"invalid signature\"}");
                return;
            }
        }
        /* Dispatch webhook to channel handler */
        {
            char ch_buf[32];
            const char *channel = webhook_path_to_channel(path, ch_buf, sizeof(ch_buf));
            (void)fprintf(stderr, "[gateway] webhook received path=%s channel=%s\n", path, channel);
            if (gw && gw->config.on_webhook) {
                gw->config.on_webhook(channel, body, body_len, gw->config.on_webhook_ctx);
            }
        }
        send_response(fd, 200, "{\"received\":true}");
        return;
    }

    send_response(fd, 404, "{\"error\":\"not found\"}");
}
#endif

sc_error_t sc_gateway_run(sc_allocator_t *alloc,
    const char *host, uint16_t port,
    const sc_gateway_config_t *config) {
    (void)host;
    (void)port;
    if (!alloc) return SC_ERR_INVALID_ARGUMENT;

    sc_gateway_config_t cfg = {0};
    if (config) {
        memcpy(&cfg, config, sizeof(cfg));
    }
    if (!cfg.host) cfg.host = "0.0.0.0";
    if (cfg.port == 0) cfg.port = SC_GATEWAY_DEFAULT_PORT;
    if (cfg.max_body_size == 0) cfg.max_body_size = SC_GATEWAY_MAX_BODY_SIZE;
    if (cfg.rate_limit_per_minute == 0) cfg.rate_limit_per_minute = SC_GATEWAY_RATE_LIMIT_PER_MIN;

    if (cfg.test_mode) {
        sc_health_mark_ok("gateway");
        return SC_OK;
    }

#ifdef SC_GATEWAY_POSIX
    sc_gateway_state_t *gw = NULL;
    int fd = -1;
    char *body_buf = NULL;
    sc_error_t err = SC_OK;

    gw = (sc_gateway_state_t *)alloc->alloc(
        alloc->ctx, sizeof(sc_gateway_state_t));
    if (!gw) {
        err = SC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(gw, 0, sizeof(*gw));
    gw->alloc = alloc;
    gw->config = cfg;

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

    gw->listen_fd = fd;
    gw->running = true;
    sc_health_mark_ok("gateway");

    body_buf = (char *)alloc->alloc(alloc->ctx, cfg.max_body_size + 1);
    if (!body_buf) {
        err = SC_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    while (gw->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(fd, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) continue;

        char client_ip[64];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        char req[4096];
        ssize_t n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            close(client);
            continue;
        }
        req[n] = '\0';

        char *line = strtok(req, "\n");
        char method[16] = {0}, path[256] = {0};
        if (line) sscanf(line, "%15s %255s", method, path);

        size_t body_len = 0;
        char *sig_header = NULL;
        while ((line = strtok(NULL, "\n")) != NULL) {
            trim_crlf(line);
            if (line[0] == '\0') break;
            if (strncasecmp(line, "Content-Length:", 15) == 0) {
                body_len = (size_t)atoi(line + 15);
                if (body_len > cfg.max_body_size) {
                    send_response(client, 413, "{\"error\":\"body too large\"}");
                    close(client);
                    continue;
                }
            }
            if (strncasecmp(line, "X-Signature:", 12) == 0) {
                char *v = line + 12;
                while (*v == ' ') v++;
                sig_header = v;
            }
        }

        if (body_len > 0 && body_len <= cfg.max_body_size) {
            size_t got = 0;
            while (got < body_len) {
                ssize_t r = recv(client, body_buf + got, body_len - got, 0);
                if (r <= 0) break;
                got += (size_t)r;
            }
            body_buf[body_len] = '\0';
        }

        handle_request(gw, client, method, path, body_buf, body_len,
            client_ip, sig_header);
        close(client);
    }

cleanup:
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
