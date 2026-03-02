#include "seaclaw/tools/web_search_providers.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char hex_char(unsigned int v) {
    return v < 10 ? (char)('0' + v) : (char)('A' + v - 10);
}

sc_error_t sc_web_search_url_encode(sc_allocator_t *alloc,
    const char *input, size_t input_len,
    char **out, size_t *out_len)
{
    if (!alloc || !out || !out_len) return SC_ERR_INVALID_ARGUMENT;
    size_t cap = input_len * 3 + 1;
    if (cap < 64) cap = 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
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
    /* Shrink to exact size so caller can free(len+1) */
    if (j + 1 < cap) {
        char *t = (char *)alloc->realloc(alloc->ctx, buf, cap, j + 1);
        if (t) buf = t;
    }
    *out = buf;
    *out_len = j;
    return SC_OK;
}
