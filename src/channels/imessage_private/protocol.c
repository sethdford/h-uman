#include "human/channels/imessage_private/protocol.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

int hu_imessage_private_port_for_uid(long uid) {
    long port = (long)HU_IMESSAGE_PRIVATE_PORT_BASE + (uid - HU_IMESSAGE_PRIVATE_PORT_UID_ANCHOR);
    if (port < HU_IMESSAGE_PRIVATE_PORT_BASE)
        port = HU_IMESSAGE_PRIVATE_PORT_BASE;
    if (port > HU_IMESSAGE_PRIVATE_PORT_MAX)
        port = HU_IMESSAGE_PRIVATE_PORT_MAX;
    return (int)port;
}

hu_imessage_private_mode_t hu_imessage_private_mode_from_string(const char *s) {
    if (!s)
        return HU_IMESSAGE_PRIVATE_MODE_OFF;
    if (strcasecmp(s, "live") == 0)
        return HU_IMESSAGE_PRIVATE_MODE_LIVE;
    if (strcasecmp(s, "shadow") == 0)
        return HU_IMESSAGE_PRIVATE_MODE_SHADOW;
    /* "off", "", and anything unrecognized → OFF (safe default). */
    return HU_IMESSAGE_PRIVATE_MODE_OFF;
}

const char *hu_imessage_private_mode_name(hu_imessage_private_mode_t mode) {
    switch (mode) {
    case HU_IMESSAGE_PRIVATE_MODE_LIVE:
        return "live";
    case HU_IMESSAGE_PRIVATE_MODE_SHADOW:
        return "shadow";
    case HU_IMESSAGE_PRIVATE_MODE_OFF:
        break;
    }
    return "off";
}

/* ── line buffer ─────────────────────────────────────────────────────── */

void hu_imsg_line_buf_init(hu_imsg_line_buf_t *buf) {
    if (!buf)
        return;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void hu_imsg_line_buf_free(hu_imsg_line_buf_t *buf) {
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

bool hu_imsg_line_buf_append(hu_imsg_line_buf_t *buf, const char *bytes, size_t n) {
    if (!buf || (!bytes && n > 0))
        return false;
    if (n == 0)
        return true;
    static const size_t MAX_BUFFER_SIZE = 1024 * 1024;
    if (buf->len + n > MAX_BUFFER_SIZE)
        return false;
    if (buf->len + n > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : 64;
        while (new_cap < buf->len + n)
            new_cap *= 2;
        if (new_cap > MAX_BUFFER_SIZE)
            new_cap = MAX_BUFFER_SIZE;
        char *grown = (char *)realloc(buf->data, new_cap);
        if (!grown)
            return false;
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, bytes, n);
    buf->len += n;
    return true;
}

char *hu_imsg_line_buf_next(hu_imsg_line_buf_t *buf, size_t *out_len) {
    if (!buf || buf->len == 0)
        return NULL;

    for (;;) {
        /* Find the next '\n'. */
        char *nl = (char *)memchr(buf->data, '\n', buf->len);
        if (!nl)
            return NULL; /* no complete line yet */

        size_t line_len = (size_t)(nl - buf->data);
        /* Strip a single trailing '\r' (the helper sends "...\r\n"). */
        if (line_len > 0 && buf->data[line_len - 1] == '\r')
            line_len--;

        size_t consumed = (size_t)(nl - buf->data) + 1; /* include the '\n' */

        if (line_len == 0) {
            /* Empty line — drop it and look for the next one. */
            memmove(buf->data, buf->data + consumed, buf->len - consumed);
            buf->len -= consumed;
            if (buf->len == 0)
                return NULL;
            continue;
        }

        char *line = (char *)malloc(line_len + 1);
        if (!line)
            return NULL; /* leave the buffer intact so the caller can retry */
        memcpy(line, buf->data, line_len);
        line[line_len] = '\0';

        memmove(buf->data, buf->data + consumed, buf->len - consumed);
        buf->len -= consumed;

        if (out_len)
            *out_len = line_len;
        return line;
    }
}
