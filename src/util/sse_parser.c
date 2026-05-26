/* src/util/sse_parser.c — see include/human/util/sse_parser.h. */

#include "human/util/sse_parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HU_SSE_INITIAL_CAP ((size_t)256)
#define HU_SSE_MAX_EVENT   ((size_t)64 * 1024) /* defensive cap per-event */

struct hu_sse_parser {
    hu_allocator_t *alloc;
    char *buf;
    size_t len;
    size_t cap;
};

hu_error_t hu_sse_parser_init(hu_allocator_t *alloc, hu_sse_parser_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_sse_parser_t *p = (hu_sse_parser_t *)alloc->alloc(alloc->ctx, sizeof(*p));
    if (!p)
        return HU_ERR_OUT_OF_MEMORY;
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    p->cap = HU_SSE_INITIAL_CAP;
    p->buf = (char *)alloc->alloc(alloc->ctx, p->cap);
    if (!p->buf) {
        alloc->free(alloc->ctx, p, sizeof(*p));
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = p;
    return HU_OK;
}

void hu_sse_parser_free(hu_sse_parser_t *p) {
    if (!p)
        return;
    if (p->buf)
        p->alloc->free(p->alloc->ctx, p->buf, p->cap);
    p->alloc->free(p->alloc->ctx, p, sizeof(*p));
}

static hu_error_t ensure_capacity(hu_sse_parser_t *p, size_t needed) {
    if (needed <= p->cap)
        return HU_OK;
    if (needed > HU_SSE_MAX_EVENT)
        return HU_ERR_OUT_OF_MEMORY; /* exceeds hard cap */
    size_t new_cap = p->cap;
    while (new_cap < needed) {
        new_cap *= (size_t)2;
        if (new_cap > HU_SSE_MAX_EVENT)
            new_cap = HU_SSE_MAX_EVENT;
    }
    char *new_buf = (char *)p->alloc->alloc(p->alloc->ctx, new_cap);
    if (!new_buf)
        return HU_ERR_OUT_OF_MEMORY;
    if (p->len > 0)
        memcpy(new_buf, p->buf, p->len);
    p->alloc->free(p->alloc->ctx, p->buf, p->cap);
    p->buf = new_buf;
    p->cap = new_cap;
    return HU_OK;
}

hu_error_t hu_sse_parser_push(hu_sse_parser_t *p, const char *bytes, size_t n) {
    if (!p)
        return HU_ERR_INVALID_ARGUMENT;
    if (n == 0 || !bytes)
        return HU_OK;
    hu_error_t e = ensure_capacity(p, p->len + n);
    if (e != HU_OK)
        return e;
    memcpy(p->buf + p->len, bytes, n);
    p->len += n;
    return HU_OK;
}

/* Find the byte offset immediately after the first blank-line terminator
 * ("\n\n" or "\r\n\r\n") in [buf, buf+len). Returns 0 if not found. */
static size_t find_event_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n')
            return i + 2;
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

/* Parse one event's body (bytes [event, event+event_len) including the
 * trailing blank-line terminator) and accumulate any `data:` lines into
 * a newly-allocated output buffer. Returns:
 *   HU_OK            — at least one data line found; *out / *out_len set
 *   HU_ERR_NOT_FOUND — no data lines (event was all comments / ignored
 *                      fields); caller should silently skip this event
 *   HU_ERR_OUT_OF_MEMORY — alloc for output buffer failed */
static hu_error_t parse_event_into(const char *event, size_t event_len, hu_allocator_t *alloc,
                                   char **out, size_t *out_len) {
    char *data = NULL;
    size_t data_len = 0;
    size_t data_cap = 0;
    bool found_data = false;

    size_t pos = 0;
    while (pos < event_len) {
        /* Line spans [pos, line_end), terminated by '\n' (or end-of-event). */
        size_t line_end = pos;
        while (line_end < event_len && event[line_end] != '\n')
            line_end++;
        /* Optional trailing '\r' before the '\n'. */
        size_t content_end = line_end;
        if (content_end > pos && event[content_end - 1] == '\r')
            content_end--;
        size_t line_len = content_end - pos;

        if (line_len == 0) {
            /* Blank line — end of this event. */
            break;
        }
        if (event[pos] == ':') {
            /* Comment line per SSE spec — ignore. */
        } else if (line_len >= 5 && memcmp(event + pos, "data:", 5) == 0) {
            const char *payload = event + pos + 5;
            size_t payload_len = line_len - 5;
            /* SSE spec: strip ONE optional leading space after the colon. */
            if (payload_len > 0 && payload[0] == ' ') {
                payload++;
                payload_len--;
            }
            /* Need room for: existing data + ('\n' if not first) + payload + '\0' */
            size_t need = data_len + (found_data ? 1u : 0u) + payload_len + 1u;
            if (need > data_cap) {
                size_t new_cap = data_cap ? data_cap : (size_t)64;
                while (new_cap < need)
                    new_cap *= (size_t)2;
                char *nd = (char *)alloc->alloc(alloc->ctx, new_cap);
                if (!nd) {
                    if (data)
                        alloc->free(alloc->ctx, data, data_cap);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                if (data) {
                    memcpy(nd, data, data_len);
                    alloc->free(alloc->ctx, data, data_cap);
                }
                data = nd;
                data_cap = new_cap;
            }
            if (found_data)
                data[data_len++] = '\n';
            if (payload_len > 0)
                memcpy(data + data_len, payload, payload_len);
            data_len += payload_len;
            found_data = true;
        }
        /* Other field lines (event:, id:, retry:, unknown) are parsed-and-ignored. */

        pos = line_end + 1; /* skip past the '\n' */
    }

    if (!found_data) {
        if (data)
            alloc->free(alloc->ctx, data, data_cap);
        return HU_ERR_NOT_FOUND;
    }
    data[data_len] = '\0';
    *out = data;
    *out_len = data_len;
    return HU_OK;
}

hu_error_t hu_sse_parser_pop_event(hu_sse_parser_t *p, char **out_data, size_t *out_data_len) {
    if (!p || !out_data || !out_data_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_data_len = 0;

    /* Loop because comment-only / metadata-only events should be
     * silently skipped while still consuming buffer bytes. */
    for (;;) {
        size_t end = find_event_end(p->buf, p->len);
        if (end == 0)
            return HU_ERR_NOT_FOUND;

        char *data = NULL;
        size_t data_len = 0;
        hu_error_t pe = parse_event_into(p->buf, end, p->alloc, &data, &data_len);

        /* Always slide the consumed event off the front of the buffer,
         * regardless of whether it produced output. Even on OOM we
         * surface the error AFTER consuming the event — the alternative
         * (retain the un-parseable event forever) would block all
         * subsequent events. */
        size_t remaining = p->len - end;
        if (remaining > 0)
            memmove(p->buf, p->buf + end, remaining);
        p->len = remaining;

        if (pe == HU_OK) {
            *out_data = data;
            *out_data_len = data_len;
            return HU_OK;
        }
        if (pe == HU_ERR_OUT_OF_MEMORY)
            return pe;
        /* pe == HU_ERR_NOT_FOUND: event had no data: lines. Skip and
         * try the next one. */
    }
}

size_t hu_sse_parser_buffered_bytes(const hu_sse_parser_t *p) {
    return p ? p->len : 0;
}
