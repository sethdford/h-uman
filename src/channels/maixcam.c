#include "human/channels/maixcam.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include <stdint.h>
#if defined(__linux__) && !HU_IS_TEST
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hu_maixcam_ctx {
    hu_allocator_t *alloc;
    char *host;
    uint16_t port;
    bool running;
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    struct {
        char session_key[128];
        char content[4096];
    } mock_msgs[8];
    size_t mock_count;
#endif
} hu_maixcam_ctx_t;

static hu_error_t maixcam_start(void *ctx) {
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void maixcam_stop(void *ctx) {
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t maixcam_send(void *ctx, const char *target, size_t target_len,
                               const char *message, size_t message_len, const char *const *media,
                               size_t media_count) {
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
#if HU_IS_TEST
    {
        hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ctx;
        size_t len = message_len > 4095 ? 4095 : message_len;
        if (message && len > 0)
            memcpy(c->last_message, message, len);
        c->last_message[len] = '\0';
        c->last_message_len = len;
        return HU_OK;
    }
#else
#if defined(__linux__)
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ctx;
    if (!c || !c->host || c->host[0] == '\0' || strncmp(c->host, "/dev/", 5) != 0) {
        return HU_ERR_NOT_SUPPORTED; /* No serial device configured */
    }
    int fd = open(c->host, O_WRONLY | O_NOCTTY);
    if (fd < 0)
        return HU_ERR_IO;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return HU_ERR_IO;
    }
    cfsetospeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return HU_ERR_IO;
    }
    char buf[4096];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"message\",\"content\":\"%.*s\"}\n",
                     (int)message_len, message);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        ssize_t w = write(fd, buf, (size_t)n);
        close(fd);
        return (w > 0) ? HU_OK : HU_ERR_IO;
    }
    close(fd);
    return HU_ERR_IO;
#else
    (void)ctx;
    (void)message;
    (void)message_len;
    return HU_ERR_NOT_SUPPORTED; /* MaixCam: Linux only */
#endif
#endif
}

static const char *maixcam_name(void *ctx) {
    (void)ctx;
    return "maixcam";
}
static bool maixcam_health_check(void *ctx) {
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ctx;
    return c && c->running;
}

static const hu_channel_vtable_t maixcam_vtable = {
    .start = maixcam_start,
    .stop = maixcam_stop,
    .send = maixcam_send,
    .name = maixcam_name,
    .health_check = maixcam_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_maixcam_create(hu_allocator_t *alloc, const char *host, size_t host_len,
                             uint16_t port, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->port = port;
    if (host && host_len > 0) {
        c->host = (char *)alloc->alloc(alloc->ctx, host_len + 1);
        if (!c->host) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->host, host, host_len);
        c->host[host_len] = '\0';
    } else {
        c->host = hu_strndup(alloc, "0.0.0.0", 7);
        if (!c->host) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    out->ctx = c;
    out->vtable = &maixcam_vtable;
    return HU_OK;
}

void hu_maixcam_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ch->ctx;
        if (c->alloc && c->host)
            c->alloc->free(c->alloc->ctx, c->host, strlen(c->host) + 1);
        if (c->alloc)
            c->alloc->free(c->alloc->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

#if HU_IS_TEST
hu_error_t hu_maixcam_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                       size_t session_key_len, const char *content,
                                       size_t content_len) {
    if (!ch || !ch->ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ch->ctx;
    if (c->mock_count >= 8)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = c->mock_count++;
    size_t sk = session_key_len > 127 ? 127 : session_key_len;
    size_t ct = content_len > 4095 ? 4095 : content_len;
    if (session_key && sk > 0)
        memcpy(c->mock_msgs[i].session_key, session_key, sk);
    c->mock_msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(c->mock_msgs[i].content, content, ct);
    c->mock_msgs[i].content[ct] = '\0';
    return HU_OK;
}
const char *hu_maixcam_test_get_last_message(hu_channel_t *ch, size_t *out_len) {
    if (!ch || !ch->ctx)
        return NULL;
    hu_maixcam_ctx_t *c = (hu_maixcam_ctx_t *)ch->ctx;
    if (out_len)
        *out_len = c->last_message_len;
    return c->last_message;
}
#endif
