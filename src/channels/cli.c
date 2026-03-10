#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_LINENOISE
#include "linenoise.h"
#elif defined(HU_ENABLE_READLINE)
#include <readline/history.h>
#include <readline/readline.h>
#endif

typedef struct hu_cli_ctx {
    hu_allocator_t *alloc;
    bool running;
} hu_cli_ctx_t;

static hu_error_t cli_start(void *ctx) {
    hu_cli_ctx_t *c = (hu_cli_ctx_t *)ctx;
    if (!c)
        return HU_ERR_INVALID_ARGUMENT;
    c->running = true;
    return HU_OK;
}

static void cli_stop(void *ctx) {
    hu_cli_ctx_t *c = (hu_cli_ctx_t *)ctx;
    if (c)
        c->running = false;
}

static hu_error_t cli_send(void *ctx, const char *target, size_t target_len, const char *message,
                           size_t message_len, const char *const *media, size_t media_count) {
    (void)ctx;
    (void)target;
    (void)target_len;
    (void)media;
    (void)media_count;
    if (message && message_len > 0) {
        fwrite(message, 1, message_len, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return HU_OK;
}

static const char *cli_name(void *ctx) {
    (void)ctx;
    return "cli";
}

static bool cli_health_check(void *ctx) {
    (void)ctx;
    return true;
}

static const hu_channel_vtable_t cli_vtable = {
    .start = cli_start,
    .stop = cli_stop,
    .send = cli_send,
    .name = cli_name,
    .health_check = cli_health_check,
    .send_event = NULL,
    .start_typing = NULL,
    .stop_typing = NULL,
};

hu_error_t hu_cli_create(hu_allocator_t *alloc, hu_channel_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_cli_ctx_t *c = (hu_cli_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->running = false;
    out->ctx = c;
    out->vtable = &cli_vtable;
    return HU_OK;
}

void hu_cli_destroy(hu_channel_t *ch) {
    if (ch && ch->ctx) {
        hu_cli_ctx_t *c = (hu_cli_ctx_t *)ch->ctx;
        hu_allocator_t *a = c->alloc;
        if (a)
            a->free(a->ctx, c, sizeof(*c));
        ch->ctx = NULL;
        ch->vtable = NULL;
    }
}

char *hu_cli_readline(hu_allocator_t *alloc, size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;

#ifdef HU_ENABLE_LINENOISE
    char *ln = linenoise("");
    if (!ln)
        return NULL;
    size_t len = strlen(ln);
    if (len > 0)
        linenoiseHistoryAdd(ln);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) {
        free(ln);
        return NULL;
    }
    memcpy(buf, ln, len + 1);
    free(ln);
    *out_len = len;
    return buf;
#elif defined(HU_ENABLE_READLINE)
    char *rl = readline("");
    if (!rl)
        return NULL;
    size_t len = strlen(rl);
    if (len > 0)
        add_history(rl);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) {
        free(rl);
        return NULL;
    }
    memcpy(buf, rl, len + 1);
    free(rl);
    *out_len = len;
    return buf;
#else
    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;
    size_t len = 0;
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            cap *= 2;
            char *n = (char *)alloc->realloc(alloc->ctx, buf, len, cap);
            if (!n) {
                alloc->free(alloc->ctx, buf, len);
                return NULL;
            }
            buf = n;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    *out_len = len;
    if (len == 0 && c == EOF) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    return buf;
#endif
}

bool hu_cli_is_quit_command(const char *line, size_t len) {
    if (!line || len == 0)
        return false;
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= len)
        return false;
    /* exit with optional trailing spaces */
    if (len - i >= 4 && (line[i] == 'e' || line[i] == 'E') &&
        (line[i + 1] == 'x' || line[i + 1] == 'X') && (line[i + 2] == 'i' || line[i + 2] == 'I') &&
        (line[i + 3] == 't' || line[i + 3] == 'T')) {
        size_t j = i + 4;
        while (j < len && (line[j] == ' ' || line[j] == '\t'))
            j++;
        if (j >= len)
            return true;
    }
    /* quit with optional trailing spaces */
    if (len - i >= 4 && (line[i] == 'q' || line[i] == 'Q') &&
        (line[i + 1] == 'u' || line[i + 1] == 'U') && (line[i + 2] == 'i' || line[i + 2] == 'I') &&
        (line[i + 3] == 't' || line[i + 3] == 'T')) {
        size_t j = i + 4;
        while (j < len && (line[j] == ' ' || line[j] == '\t'))
            j++;
        if (j >= len)
            return true;
    }
    if (len - i == 2 && line[i] == ':' && line[i + 1] == 'q')
        return true;
    return false;
}
