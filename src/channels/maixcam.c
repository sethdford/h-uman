#include "seaclaw/channels/maixcam.h"
#include <stdint.h>
#if defined(__linux__) && !SC_IS_TEST
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct sc_maixcam_ctx {
    sc_allocator_t *alloc;
    char *host;
    uint16_t port;
    bool running;
} sc_maixcam_ctx_t;

static sc_error_t maixcam_start(void *ctx) {
    sc_maixcam_ctx_t *c = (sc_maixcam_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void maixcam_stop(void *ctx) {
    sc_maixcam_ctx_t *c = (sc_maixcam_ctx_t *)ctx;
    if (c) c->running = false;
}

static sc_error_t maixcam_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)target; (void)target_len; (void)media; (void)media_count;
#if SC_IS_TEST
    (void)ctx;
    (void)message;
    (void)message_len;
    return SC_OK;
#else
#if defined(__linux__)
    sc_maixcam_ctx_t *c = (sc_maixcam_ctx_t *)ctx;
    if (!c || !c->host || c->host[0] == '\0' || strncmp(c->host, "/dev/", 5) != 0) {
        return SC_ERR_NOT_SUPPORTED; /* No serial device configured */
    }
    int fd = open(c->host, O_WRONLY | O_NOCTTY);
    if (fd < 0) return SC_ERR_IO;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return SC_ERR_IO;
    }
    cfsetospeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return SC_ERR_IO;
    }
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"message\",\"content\":\"%.*s\"}\n",
        (int)message_len, message);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        ssize_t w = write(fd, buf, (size_t)n);
        close(fd);
        return (w > 0) ? SC_OK : SC_ERR_IO;
    }
    close(fd);
    return SC_ERR_IO;
#else
    (void)ctx;
    (void)message;
    (void)message_len;
    return SC_ERR_NOT_SUPPORTED; /* MaixCam: Linux only */
#endif
#endif
}

static const char *maixcam_name(void *ctx) { (void)ctx; return "maixcam"; }
static bool maixcam_health_check(void *ctx) {
    sc_maixcam_ctx_t *c = (sc_maixcam_ctx_t *)ctx;
    return c && c->running;
}

static const sc_channel_vtable_t maixcam_vtable = {
    .start = maixcam_start, .stop = maixcam_stop, .send = maixcam_send,
    .name = maixcam_name, .health_check = maixcam_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_maixcam_create(sc_allocator_t *alloc,
    const char *host, size_t host_len,
    uint16_t port,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_maixcam_ctx_t *c = (sc_maixcam_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->port = port;
    if (host && host_len > 0) {
        c->host = (char *)malloc(host_len + 1);
        if (c->host) {
            memcpy(c->host, host, host_len);
            c->host[host_len] = '\0';
        }
    } else {
        c->host = strdup("0.0.0.0");
    }
    out->ctx = c;
    out->vtable = &maixcam_vtable;
    return SC_OK;
}

void sc_maixcam_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_maixcam_ctx_t *c = (sc_maixcam_ctx_t *)ch->ctx;
        if (c->host) free(c->host);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}
