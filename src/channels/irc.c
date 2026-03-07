#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IRC_SESSION_KEY_MAX 127
#define IRC_CONTENT_MAX     4095
#define IRC_RECV_BUF_SIZE   4096

typedef struct sc_irc_ctx {
    sc_allocator_t *alloc;
    char *server;
    size_t server_len;
    uint16_t port;
    int sock_fd;
    bool connected;
    bool running;
    char *nick;
    char *channel;
    char recv_buf[IRC_RECV_BUF_SIZE];
    size_t recv_len;
#if SC_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} sc_irc_ctx_t;

static sc_error_t irc_start(void *ctx) {
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ctx;
    if (!c)
        return SC_ERR_INVALID_ARGUMENT;
#if SC_IS_TEST
    c->running = true;
    return SC_OK;
#else
    if (!c->server || c->server_len == 0)
        return SC_ERR_CHANNEL_NOT_CONFIGURED;

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

static sc_error_t irc_send(void *ctx, const char *target, size_t target_len, const char *message,
                           size_t message_len, const char *const *media, size_t media_count) {
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    {
        sc_irc_ctx_t *c = (sc_irc_ctx_t *)ctx;
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return SC_OK;
    }
#else
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ctx;
    if (!c || !c->connected || c->sock_fd < 0)
        return SC_ERR_NOT_SUPPORTED;

    /* IRC max line 512 bytes: PRIVMSG target :message\r\n */
    size_t overhead = 10 + target_len; /* "PRIVMSG " + target + " :" */
    size_t max_chunk = (512 - overhead - 2);
    if (max_chunk > 400)
        max_chunk = 400;

    size_t sent = 0;
    while (sent < message_len) {
        size_t chunk_len = message_len - sent;
        if (chunk_len > max_chunk)
            chunk_len = max_chunk;

        char line[512];
        int n = snprintf(line, sizeof(line), "PRIVMSG %.*s :%.*s\r\n", (int)target_len, target,
                         (int)chunk_len, message + sent);
        if (n <= 0 || (size_t)n >= sizeof(line))
            return SC_ERR_IO;

        ssize_t w = write(c->sock_fd, line, (size_t)n);
        if (w < 0)
            return SC_ERR_IO;
        sent += (size_t)chunk_len;
    }
    return SC_OK;
#endif
}

static const char *irc_name(void *ctx) {
    (void)ctx;
    return "irc";
}
static bool irc_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const sc_channel_vtable_t irc_vtable = {
    .start = irc_start,
    .stop = irc_stop,
    .send = irc_send,
    .name = irc_name,
    .health_check = irc_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

sc_error_t sc_irc_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                       size_t max_msgs, size_t *out_count) {
    sc_irc_ctx_t *ctx = (sc_irc_ctx_t *)channel_ctx;
    if (!ctx || !msgs || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if SC_IS_TEST
    {
        size_t n = ctx->mock_count < max_msgs ? ctx->mock_count : max_msgs;
        for (size_t i = 0; i < n; i++) {
            memcpy(msgs[i].session_key, ctx->mock_msgs[i].session_key, 128);
            memcpy(msgs[i].content, ctx->mock_msgs[i].content, 4096);
        }
        *out_count = n;
        ctx->mock_count = 0;
        return SC_OK;
    }
#else
#ifdef SC_GATEWAY_POSIX
    if (!ctx->connected || ctx->sock_fd < 0 || !ctx->running)
        return SC_OK;
    struct timeval tv = {0, 100000};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx->sock_fd, &rfds);
    int ready = select(ctx->sock_fd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0)
        return SC_OK;
    size_t space = IRC_RECV_BUF_SIZE - ctx->recv_len - 1;
    if (space == 0)
        ctx->recv_len = 0, space = IRC_RECV_BUF_SIZE - 1;
    ssize_t n = recv(ctx->sock_fd, ctx->recv_buf + ctx->recv_len, space, 0);
    if (n <= 0)
        return SC_OK;
    ctx->recv_len += (size_t)n;
    ctx->recv_buf[ctx->recv_len] = '\0';
    size_t cnt = 0;
    char *line_start = ctx->recv_buf;
    char *line_end;
    while ((line_end = strstr(line_start, "\r\n")) != NULL && cnt < max_msgs) {
        *line_end = '\0';
        if (strncmp(line_start, "PING ", 5) == 0) {
            char pong[512];
            int pn = snprintf(pong, sizeof(pong), "PONG %s\r\n", line_start + 5);
            if (pn > 0)
                send(ctx->sock_fd, pong, (size_t)pn, 0);
        } else {
            char *privmsg = strstr(line_start, "PRIVMSG ");
            if (privmsg) {
                char *nick_end = strchr(line_start, '!');
                const char *sender = line_start + 1;
                size_t sender_len = nick_end ? (size_t)(nick_end - sender) : 0;
                char *colon = strchr(privmsg, ':');
                if (colon && sender_len > 0) {
                    const char *text = colon + 1;
                    size_t text_len = strlen(text);
                    if (text_len > 0) {
                        size_t sk =
                            sender_len < IRC_SESSION_KEY_MAX ? sender_len : IRC_SESSION_KEY_MAX;
                        memcpy(msgs[cnt].session_key, sender, sk);
                        msgs[cnt].session_key[sk] = '\0';
                        size_t ct = text_len < IRC_CONTENT_MAX ? text_len : IRC_CONTENT_MAX;
                        memcpy(msgs[cnt].content, text, ct);
                        msgs[cnt].content[ct] = '\0';
                        cnt++;
                    }
                }
            }
        }
        line_start = line_end + 2;
    }
    if (line_start > ctx->recv_buf) {
        size_t remaining = ctx->recv_len - (size_t)(line_start - ctx->recv_buf);
        if (remaining > 0)
            memmove(ctx->recv_buf, line_start, remaining);
        ctx->recv_len = remaining;
    }
    *out_count = cnt;
    (void)alloc;
    return SC_OK;
#else
    (void)alloc;
    (void)max_msgs;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

sc_error_t sc_irc_create(sc_allocator_t *alloc, const char *server, size_t server_len,
                         uint16_t port, sc_channel_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (server && server_len > 0) {
        c->server = (char *)malloc(server_len + 1);
        if (!c->server) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return SC_ERR_OUT_OF_MEMORY;
        }
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
        sc_allocator_t *a = c->alloc;
#if !SC_IS_TEST
        if (c->connected && c->sock_fd >= 0) {
            close(c->sock_fd);
            c->sock_fd = -1;
        }
        c->connected = false;
#endif
        if (c->server)
            free(c->server);
        a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if SC_IS_TEST
sc_error_t sc_irc_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                   size_t session_key_len, const char *content,
                                   size_t content_len) {
    if (!ch || !ch->ctx)
        return SC_ERR_INVALID_ARGUMENT;
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return SC_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return SC_OK;
}
const char *sc_irc_test_get_last_message(sc_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    sc_irc_ctx_t *c = (sc_irc_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
