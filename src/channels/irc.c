#include "seaclaw/channel.h"
#include <stdint.h>
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

typedef struct sc_irc_ctx {
    char *server;
    size_t server_len;
    uint16_t port;
    int sock_fd;
    bool connected;
    bool running;
} sc_irc_ctx_t;

static sc_error_t irc_start(void *ctx) {
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    c->running = true;
    return SC_OK;
#else
    if (!c->server || c->server_len == 0) return SC_ERR_CHANNEL_NOT_CONFIGURED;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", c->port);
    if (getaddrinfo(c->server, port_str, &hints, &res) != 0)
        return SC_ERR_IO;

    c->sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->sock_fd < 0) {
        freeaddrinfo(res);
        return SC_ERR_IO;
    }
    if (connect(c->sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(c->sock_fd);
        c->sock_fd = -1;
        freeaddrinfo(res);
        return SC_ERR_IO;
    }
    freeaddrinfo(res);
    c->connected = true;
    c->running = true;
    return SC_OK;
#endif
}

static void irc_stop(void *ctx) {
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ctx;
    if (c) {
#if !SC_IS_TEST
        if (c->connected && c->sock_fd >= 0) {
            close(c->sock_fd);
            c->sock_fd = -1;
        }
        c->connected = false;
#endif
        c->running = false;
    }
}

static sc_error_t irc_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)message;
    (void)message_len;
    return SC_OK;
#else
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ctx;
    if (!c || !c->connected || c->sock_fd < 0)
        return SC_ERR_NOT_SUPPORTED;

    /* IRC max line 512 bytes: PRIVMSG target :message\r\n */
    size_t overhead = 10 + target_len; /* "PRIVMSG " + target + " :" */
    size_t max_chunk = (512 - overhead - 2);
    if (max_chunk > 400) max_chunk = 400;

    size_t sent = 0;
    while (sent < message_len) {
        size_t chunk_len = message_len - sent;
        if (chunk_len > max_chunk) chunk_len = max_chunk;

        char line[512];
        int n = snprintf(line, sizeof(line), "PRIVMSG %.*s :%.*s\r\n",
            (int)target_len, target, (int)chunk_len, message + sent);
        if (n <= 0 || (size_t)n >= sizeof(line)) return SC_ERR_IO;

        ssize_t w = write(c->sock_fd, line, (size_t)n);
        if (w < 0) return SC_ERR_IO;
        sent += (size_t)chunk_len;
    }
    return SC_OK;
#endif
}

static const char *irc_name(void *ctx) { (void)ctx; return "irc"; }
static bool irc_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t irc_vtable = {
    .start = irc_start, .stop = irc_stop, .send = irc_send,
    .name = irc_name, .health_check = irc_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_irc_create(sc_allocator_t *alloc,
    const char *server, size_t server_len,
    uint16_t port,
    sc_channel_t *out)
{
    (void)alloc;
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (server && server_len > 0) {
        c->server = (char *)malloc(server_len + 1);
        if (!c->server) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->server, server, server_len);
        c->server[server_len] = '\0';
        c->server_len = server_len;
    }
    c->port = port > 0 ? port : 6667;
    c->sock_fd = -1;
    c->connected = false;
    out->ctx = c;
    out->vtable = &irc_vtable;
    return SC_OK;
}

void sc_irc_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_irc_ctx_t *c = (sc_irc_ctx_t *)ch->ctx;
#if !SC_IS_TEST
        if (c->connected && c->sock_fd >= 0) {
            close(c->sock_fd);
            c->sock_fd = -1;
        }
        c->connected = false;
#endif
        if (c->server) free(c->server);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
