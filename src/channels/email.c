#include "seaclaw/channels/email.h"
#include "seaclaw/core/process_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define SC_EMAIL_LAST_MSG_SIZE 4096

typedef struct sc_email_ctx {
    sc_allocator_t *alloc;
    char *smtp_host;
    size_t smtp_host_len;
    uint16_t smtp_port;
    char *from_address;
    size_t from_len;
    bool running;
#if SC_IS_TEST
    char last_message[SC_EMAIL_LAST_MSG_SIZE];
    size_t last_message_len;
#endif
} sc_email_ctx_t;

static sc_error_t email_start(void *ctx) {
    sc_email_ctx_t *c = (sc_email_ctx_t *)ctx;
    if (!c) return SC_ERR_INVALID_ARGUMENT;
    c->running = true;
    return SC_OK;
}

static void email_stop(void *ctx) {
    sc_email_ctx_t *c = (sc_email_ctx_t *)ctx;
    if (c) c->running = false;
}

#if !SC_IS_TEST
static const char *email_from(sc_email_ctx_t *c) {
    if (c->from_address && c->from_len > 0)
        return c->from_address;
    return "seaclaw@localhost";
}
#endif

static sc_error_t email_send(void *ctx,
    const char *target, size_t target_len,
    const char *message, size_t message_len,
    const char *const *media, size_t media_count)
{
    (void)media;
    (void)media_count;
#if SC_IS_TEST
    (void)target;
    (void)target_len;
    sc_email_ctx_t *c = (sc_email_ctx_t *)ctx;
    if (c && message && message_len > 0) {
        size_t copy = message_len;
        if (copy >= SC_EMAIL_LAST_MSG_SIZE) copy = SC_EMAIL_LAST_MSG_SIZE - 1;
        memcpy(c->last_message, message, copy);
        c->last_message[copy] = '\0';
        c->last_message_len = copy;
    }
    return SC_OK;
#else
    sc_email_ctx_t *c = (sc_email_ctx_t *)ctx;
    if (!c || !c->alloc) return SC_ERR_INVALID_ARGUMENT;
    if (!c->smtp_host || c->smtp_host_len == 0)
        return SC_ERR_NOT_SUPPORTED;

    const char *from = email_from(c);

    /* Write email to temp file (MIME plain text) */
    char tmppath[] = "/tmp/sc_email_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return SC_ERR_IO;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmppath);
        return SC_ERR_IO;
    }
    int n = fprintf(f,
        "From: %s\r\nTo: %.*s\r\nSubject: seaclaw\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n%.*s",
        from, (int)target_len, target, (int)message_len, message);
    fclose(f);
    if (n < 0) {
        unlink(tmppath);
        return SC_ERR_IO;
    }

    /* Null-terminate target for argv */
    char *target_str = (char *)c->alloc->alloc(c->alloc->ctx, target_len + 1);
    if (!target_str) {
        unlink(tmppath);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(target_str, target, target_len);
    target_str[target_len] = '\0';

    char smtp_url[512];
    snprintf(smtp_url, sizeof(smtp_url), "smtp://%.*s:%u",
        (int)c->smtp_host_len, c->smtp_host, c->smtp_port);

    const char *argv[] = {
        "curl", "--url", smtp_url,
        "--mail-from", from,
        "--mail-rcpt", target_str,
        "--upload-file", tmppath,
        NULL
    };

    sc_run_result_t run = {0};
    sc_error_t err = sc_process_run(c->alloc, argv, NULL, 4096, &run);
    c->alloc->free(c->alloc->ctx, target_str, target_len + 1);
    unlink(tmppath);
    sc_run_result_free(c->alloc, &run);
    return (err == SC_OK && run.success) ? SC_OK : SC_ERR_IO;
#endif
}

static const char *email_name(void *ctx) { (void)ctx; return "email"; }
static bool email_health_check(void *ctx) { (void)ctx; return true; }

static const sc_channel_vtable_t email_vtable = {
    .start = email_start, .stop = email_stop, .send = email_send,
    .name = email_name, .health_check = email_health_check,
    .send_event = NULL, .start_typing = NULL, .stop_typing = NULL,
};

sc_error_t sc_email_create(sc_allocator_t *alloc,
    const char *smtp_host, size_t smtp_host_len,
    uint16_t smtp_port,
    const char *from_address, size_t from_len,
    sc_channel_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_email_ctx_t *c = (sc_email_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (smtp_host && smtp_host_len > 0) {
        c->smtp_host = (char *)malloc(smtp_host_len + 1);
        if (!c->smtp_host) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->smtp_host, smtp_host, smtp_host_len);
        c->smtp_host[smtp_host_len] = '\0';
        c->smtp_host_len = smtp_host_len;
    }
    c->smtp_port = smtp_port > 0 ? smtp_port : 587;
    if (from_address && from_len > 0) {
        c->from_address = (char *)malloc(from_len + 1);
        if (!c->from_address) {
            if (c->smtp_host) free(c->smtp_host);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->from_address, from_address, from_len);
        c->from_address[from_len] = '\0';
        c->from_len = from_len;
    }
    out->ctx = c;
    out->vtable = &email_vtable;
    return SC_OK;
}

void sc_email_destroy(sc_channel_t *ch) {
    if (ch && ch->ctx) {
        sc_email_ctx_t *c = (sc_email_ctx_t *)ch->ctx;
        if (c->smtp_host) free(c->smtp_host);
        if (c->from_address) free(c->from_address);
        free(c);
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

bool sc_email_is_configured(sc_channel_t *ch) {
    if (!ch || !ch->ctx) return false;
    sc_email_ctx_t *c = (sc_email_ctx_t *)ch->ctx;
    return c->smtp_host != NULL && c->smtp_host[0] != '\0'
        && c->from_address != NULL && c->from_address[0] != '\0';
}

#if SC_IS_TEST
const char *sc_email_test_last_message(sc_channel_t *ch) {
    if (!ch || !ch->ctx) return NULL;
    sc_email_ctx_t *c = (sc_email_ctx_t *)ch->ctx;
    return c->last_message_len > 0 ? c->last_message : NULL;
}
#endif
