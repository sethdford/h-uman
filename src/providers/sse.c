#include "seaclaw/providers/sse.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#define SC_SSE_PARSER_INIT_CAP 256

static bool field_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    return alen == blen && memcmp(a, b, alen) == 0;
}

sc_error_t sc_sse_parser_init(sc_sse_parser_t *p, sc_allocator_t *alloc) {
    if (!p || !alloc) return SC_ERR_INVALID_ARGUMENT;
    memset(p, 0, sizeof(*p));
    p->alloc = alloc;
    p->buffer = (char *)alloc->alloc(alloc->ctx, SC_SSE_PARSER_INIT_CAP);
    if (!p->buffer) return SC_ERR_OUT_OF_MEMORY;
    p->buf_cap = SC_SSE_PARSER_INIT_CAP;
    p->buf_len = 0;
    p->buffer[0] = '\0';
    return SC_OK;
}

void sc_sse_parser_deinit(sc_sse_parser_t *p) {
    if (!p) return;
    if (p->buffer && p->alloc) {
        p->alloc->free(p->alloc->ctx, p->buffer, p->buf_cap);
        p->buffer = NULL;
        p->buf_len = 0;
        p->buf_cap = 0;
    }
    p->alloc = NULL;
}

static void parse_field(const char *line, size_t line_len,
    const char **field_out, size_t *field_len,
    const char **value_out, size_t *value_len)
{
    const char *colon = memchr(line, ':', line_len);
    if (colon) {
        *field_out = line;
        *field_len = (size_t)(colon - line);
        const char *val = colon + 1;
        size_t val_len = line_len - (size_t)(colon - line) - 1;
        /* SSE spec: trim optional leading space(s) after colon */
        while (val_len > 0 && (*val == ' ' || *val == '\t')) {
            val++;
            val_len--;
        }
        *value_out = val;
        *value_len = val_len;
    } else {
        *field_out = line;
        *field_len = line_len;
        *value_out = line + line_len;
        *value_len = 0;
    }
}

sc_error_t sc_sse_parser_feed(sc_sse_parser_t *p,
    const char *bytes, size_t len,
    sc_sse_event_cb callback, void *userdata)
{
    if (!p || !p->alloc || !callback) return SC_ERR_INVALID_ARGUMENT;
    if (len == 0) return SC_OK;

    while (p->buf_len + len + 1 > p->buf_cap) {
        size_t new_cap = p->buf_cap ? p->buf_cap * 2 : SC_SSE_PARSER_INIT_CAP;
        while (new_cap < p->buf_len + len + 1) new_cap *= 2;
        char *nbuf = (char *)p->alloc->realloc(p->alloc->ctx, p->buffer,
            p->buf_cap, new_cap);
        if (!nbuf) return SC_ERR_OUT_OF_MEMORY;
        p->buffer = nbuf;
        p->buf_cap = new_cap;
    }
    memcpy(p->buffer + p->buf_len, bytes, len);
    p->buf_len += len;
    p->buffer[p->buf_len] = '\0';

    char *buf = p->buffer;
    size_t total = p->buf_len;
    size_t event_type_len = 7;
    const char *event_type = "message";
    size_t data_len = 0;
    size_t data_cap = 0;
    char *data = NULL;
    int has_data = 0;

    const char *p_end = buf + total;
    const char *line_start = buf;
    const char *consumed_up_to = buf;

    while (line_start < p_end) {
        const char *line_end = line_start;
        while (line_end < p_end && *line_end != '\n' &&
               !(line_end + 1 < p_end && *line_end == '\r' && line_end[1] == '\n')) {
            line_end++;
        }
        /* Incomplete line (no newline seen): keep in buffer for next feed */
        if (line_end >= p_end)
            break;
        size_t line_len = (size_t)(line_end - line_start);
        if (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;

        if (line_len == 0) {
            callback(event_type, event_type_len,
                has_data && data ? data : NULL,
                has_data ? data_len : 0, userdata);
            if (data) {
                p->alloc->free(p->alloc->ctx, data, data_cap);
                data = NULL;
            }
            data_len = 0;
            data_cap = 0;
            has_data = 0;
            event_type = "message";
            event_type_len = 7;
            consumed_up_to = line_end + 1;
            if (consumed_up_to < p_end && *(line_end) == '\r' && line_end[1] == '\n')
                consumed_up_to++;
            line_start = consumed_up_to;
            continue;
        }

        if (line_start[0] != ':') {
            const char *field, *value;
            size_t field_len, value_len;
            parse_field(line_start, line_len, &field, &field_len, &value, &value_len);

            if (field_eq(field, field_len, "data")) {
                size_t need = data_len + value_len + (has_data ? 1 : 0) + 1;
                if (need > data_cap) {
                    size_t new_cap = data_cap ? data_cap * 2 : 256;
                    while (new_cap < need) new_cap *= 2;
                    char *nd;
                    if (data) {
                        nd = (char *)p->alloc->realloc(p->alloc->ctx, data,
                            data_cap, new_cap);
                    } else {
                        nd = (char *)p->alloc->alloc(p->alloc->ctx, new_cap);
                    }
                    if (!nd) {
                        if (data) p->alloc->free(p->alloc->ctx, data, data_cap);
                        return SC_ERR_OUT_OF_MEMORY;
                    }
                    data = nd;
                    data_cap = new_cap;
                }
                if (has_data) data[data_len++] = '\n';
                memcpy(data + data_len, value, value_len);
                data_len += value_len;
                data[data_len] = '\0';
                has_data = 1;
            } else if (field_eq(field, field_len, "event")) {
                event_type = value;
                event_type_len = value_len;
            }
        }

        line_start = line_end + 1;
        if (line_start - 1 < p_end && *(line_end) == '\r' && line_end[1] == '\n')
            line_start++;
        consumed_up_to = line_start;
    }

    size_t keep = (size_t)(consumed_up_to - buf);
    if (keep < total && keep > 0) {
        memmove(buf, consumed_up_to, total - keep);
        p->buf_len = total - keep;
    } else if (keep >= total) {
        p->buf_len = 0;
    }

    if (data && !has_data) {
        p->alloc->free(p->alloc->ctx, data, data_cap);
    }
    return SC_OK;
}

/* Line-level parsing */
#define SC_SSE_DATA_PREFIX "data: "
#define SC_SSE_DATA_PREFIX_LEN 6

static void trim_right(const char *s, size_t len, size_t *new_len) {
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' '))
        len--;
    *new_len = len;
}

sc_error_t sc_sse_parse_line(sc_allocator_t *alloc, const char *line, size_t line_len,
    sc_sse_line_result_t *out)
{
    if (!line || !out) return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    trim_right(line, line_len, &line_len);
    if (line_len == 0) { out->tag = SC_SSE_SKIP; return SC_OK; }
    if (line[0] == ':') { out->tag = SC_SSE_SKIP; return SC_OK; }

    if (line_len < SC_SSE_DATA_PREFIX_LEN || memcmp(line, SC_SSE_DATA_PREFIX, SC_SSE_DATA_PREFIX_LEN) != 0) {
        out->tag = SC_SSE_SKIP;
        return SC_OK;
    }

    const char *data = line + SC_SSE_DATA_PREFIX_LEN;
    size_t data_len = line_len - SC_SSE_DATA_PREFIX_LEN;
    while (data_len > 0 && (*data == ' ' || *data == '\t')) { data++; data_len--; }

    if (data_len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        out->tag = SC_SSE_DONE;
        return SC_OK;
    }

    char *delta = sc_sse_extract_delta_content(alloc, data, data_len);
    if (!delta) {
        out->tag = SC_SSE_SKIP;
        return SC_OK;
    }
    out->tag = SC_SSE_DELTA;
    out->delta = delta;
    out->delta_len = strlen(delta);
    return SC_OK;
}

char *sc_sse_extract_delta_content(sc_allocator_t *alloc, const char *json_str, size_t json_len) {
    sc_json_value_t *parsed = NULL;
    if (sc_json_parse(alloc, json_str, json_len, &parsed) != SC_OK) return NULL;

    sc_json_value_t *choices = sc_json_object_get(parsed, "choices");
    if (!choices || choices->type != SC_JSON_ARRAY || choices->data.array.len == 0) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    sc_json_value_t *first = choices->data.array.items[0];
    if (first->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    sc_json_value_t *delta = sc_json_object_get(first, "delta");
    if (!delta || delta->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, parsed);
        return NULL;
    }

    const char *content = sc_json_get_string(delta, "content");
    char *out = NULL;
    if (content && strlen(content) > 0)
        out = sc_strndup(alloc, content, strlen(content));
    sc_json_free(alloc, parsed);
    return out;
}
