/*
 * Gmail base64url decode — shared implementation for channel and tests.
 * Gmail body.data uses base64url: - and _ instead of + and /.
 */
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

static int b64url_char_val(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}

hu_error_t base64url_decode(const char *in, size_t in_len, char *out, size_t out_cap,
                            size_t *out_len) {
    while (in_len > 0 &&
           (in[in_len - 1] == '=' || in[in_len - 1] == '\n' || in[in_len - 1] == '\r'))
        in_len--;
    size_t byte_len = (in_len * 3) / 4;
    if (out_cap < byte_len + 1)
        return HU_ERR_INVALID_ARGUMENT;
    size_t j = 0;
    for (size_t i = 0; i + 4 <= in_len; i += 4) {
        int a = b64url_char_val(in[i]);
        int b = b64url_char_val(in[i + 1]);
        int c = b64url_char_val(in[i + 2]);
        int d = b64url_char_val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (b << 12) | (c << 6) | d;
        out[j++] = (char)(val >> 16);
        out[j++] = (char)(val >> 8);
        out[j++] = (char)val;
    }
    if (in_len % 4 == 2) {
        int a = b64url_char_val(in[in_len - 2]);
        int b = b64url_char_val(in[in_len - 1]);
        if (a < 0 || b < 0)
            return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (b << 12);
        out[j++] = (char)(val >> 16);
    } else if (in_len % 4 == 3) {
        int a = b64url_char_val(in[in_len - 3]);
        int b = b64url_char_val(in[in_len - 2]);
        int c = b64url_char_val(in[in_len - 1]);
        if (a < 0 || b < 0 || c < 0)
            return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (b << 12) | (c << 6);
        out[j++] = (char)(val >> 16);
        out[j++] = (char)(val >> 8);
    }
    out[j] = '\0';
    *out_len = j;
    return HU_OK;
}
