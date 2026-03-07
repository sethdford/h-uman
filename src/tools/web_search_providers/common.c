#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/web_search_providers.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_WEB_SEARCH_BUF_INIT 4096

sc_error_t sc_web_search_format_results(sc_allocator_t *alloc, const char *query, size_t query_len,
                                        sc_json_value_t *results_array, int count,
                                        const char *title_key, const char *url_key,
                                        const char *snippet_key, sc_tool_result_t *out) {
    if (!alloc || !results_array || !out)
        return SC_ERR_INVALID_ARGUMENT;
    if (results_array->type != SC_JSON_ARRAY || results_array->data.array.len == 0) {
        *out = sc_tool_result_ok_owned(sc_strndup(alloc, "No web results found.", 20), 20);
        return SC_OK;
    }

    size_t cap = SC_WEB_SEARCH_BUF_INIT;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    int n = snprintf(buf, cap, "Results for: %.*s\n\n", (int)query_len, query);
    if (n > 0)
        len = (size_t)n;

    int max_r = count;
    if (max_r > (int)results_array->data.array.len)
        max_r = (int)results_array->data.array.len;
    for (int i = 0; i < max_r; i++) {
        sc_json_value_t *item = results_array->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;
        const char *title = sc_json_get_string(item, title_key);
        const char *url = sc_json_get_string(item, url_key);
        const char *snippet = sc_json_get_string(item, snippet_key);
        if (!title)
            title = "";
        if (!url)
            url = "";
        if (!snippet)
            snippet = "";

        char line[1024];
        int ln =
            snprintf(line, sizeof(line), "%d. %s\n   %s\n   %s\n\n", i + 1, title, url, snippet);
        if (ln <= 0 || (size_t)ln >= sizeof(line))
            continue;
        if (len + (size_t)ln < cap) {
            memcpy(buf + len, line, (size_t)ln + 1);
            len += (size_t)ln;
        } else {
            size_t new_cap = cap * 2;
            char *nbuf = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nbuf)
                break;
            buf = nbuf;
            cap = new_cap;
            memcpy(buf + len, line, (size_t)ln + 1);
            len += (size_t)ln;
        }
    }

    *out = sc_tool_result_ok_owned(buf, len);
    return SC_OK;
}

static char hex_char(unsigned int v) {
    return v < 10 ? (char)('0' + v) : (char)('A' + v - 10);
}

sc_error_t sc_web_search_url_encode(sc_allocator_t *alloc, const char *input, size_t input_len,
                                    char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    if (input_len > (SIZE_MAX - 1) / 3)
        return SC_ERR_INVALID_ARGUMENT;
    size_t cap = input_len * 3 + 1;
    if (cap < 64)
        cap = 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < input_len && j + 4 <= cap; i++) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            buf[j++] = (char)c;
        } else if (c == ' ') {
            buf[j++] = '+';
        } else {
            buf[j++] = '%';
            buf[j++] = hex_char(c >> 4);
            buf[j++] = hex_char(c & 0x0f);
        }
    }
    buf[j] = '\0';
    if (j + 1 < cap) {
        char *t = (char *)alloc->realloc(alloc->ctx, buf, cap, j + 1);
        if (!t) {
            alloc->free(alloc->ctx, buf, cap);
            return SC_ERR_OUT_OF_MEMORY;
        }
        buf = t;
    }
    *out = buf;
    *out_len = j;
    return SC_OK;
}
