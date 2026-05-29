#include "human/providers/error_classify.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bool hu_error_is_non_retryable(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    for (size_t i = 0; i < msg_len;) {
        if (!isdigit((unsigned char)msg[i])) {
            i++;
            continue;
        }
        size_t end = i;
        while (end < msg_len && isdigit((unsigned char)msg[end]))
            end++;
        if (end - i == 3) {
            char buf[4];
            buf[0] = msg[i];
            buf[1] = msg[i + 1];
            buf[2] = msg[i + 2];
            buf[3] = '\0';
            int code = (int)strtol(buf, NULL, 10);
            if (code >= 400 && code < 500 && code != 429 && code != 408)
                return true;
        }
        i = end;
    }
    return false;
}

bool hu_error_is_context_exhausted(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    size_t check_len = msg_len > 512 ? 512 : msg_len;
    bool has_context = hu_str_contains_ci(msg, check_len, "context", 7);
    bool has_token = hu_str_contains_ci(msg, check_len, "token", 5);
    if (has_context &&
        (hu_str_contains_ci(msg, check_len, "length", 6) ||
         hu_str_contains_ci(msg, check_len, "maximum", 7) ||
         hu_str_contains_ci(msg, check_len, "window", 6) || hu_str_contains_ci(msg, check_len, "exceed", 6)))
        return true;
    if (has_token &&
        (hu_str_contains_ci(msg, check_len, "limit", 5) ||
         hu_str_contains_ci(msg, check_len, "too many", 8) ||
         hu_str_contains_ci(msg, check_len, "maximum", 7) || hu_str_contains_ci(msg, check_len, "exceed", 6)))
        return true;
    if (hu_str_contains_ci(msg, check_len, "413", 3) && hu_str_contains_ci(msg, check_len, "too large", 9))
        return true;
    return false;
}

bool hu_error_is_rate_limited(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    size_t check_len = msg_len > 512 ? 512 : msg_len;
    if (hu_str_contains_ci(msg, check_len, "ratelimited", 11) ||
        hu_str_contains_ci(msg, check_len, "rate limited", 12) ||
        hu_str_contains_ci(msg, check_len, "rate_limit", 10) ||
        hu_str_contains_ci(msg, check_len, "too many requests", 17) ||
        hu_str_contains_ci(msg, check_len, "quota exceeded", 14) ||
        hu_str_contains_ci(msg, check_len, "throttle", 8))
        return true;
    return hu_str_contains_ci(msg, check_len, "429", 3) &&
           (hu_str_contains_ci(msg, check_len, "rate", 4) || hu_str_contains_ci(msg, check_len, "limit", 5) ||
            hu_str_contains_ci(msg, check_len, "too many", 8));
}

uint64_t hu_error_parse_retry_after_ms(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return 0;
    static const char *prefixes[] = {"retry-after:", "retry_after:", "retry-after ",
                                     "retry_after "};
    size_t check_len = msg_len > 4096 ? 4096 : msg_len;

    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        size_t plen = strlen(prefixes[p]);
        for (size_t i = 0; i + plen <= check_len; i++) {
            if (strncasecmp(msg + i, prefixes[p], plen) != 0)
                continue;
            size_t start = i + plen;
            while (start < check_len && (msg[start] == ' ' || msg[start] == '\t'))
                start++;
            size_t end = start;
            while (end < check_len && (isdigit((unsigned char)msg[end]) || msg[end] == '.'))
                end++;
            if (end > start) {
                char buf[32];
                size_t copy = end - start < 31 ? end - start : 31;
                memcpy(buf, msg + start, copy);
                buf[copy] = '\0';
                double secs = atof(buf);
                if (secs >= 0.0)
                    return (uint64_t)(secs * 1000.0);
            }
        }
    }
    return 0;
}

bool hu_error_is_rate_limited_text(const char *text, size_t text_len) {
    return hu_error_is_rate_limited(text, text_len);
}

bool hu_error_is_context_exhausted_text(const char *text, size_t text_len) {
    return hu_error_is_context_exhausted(text, text_len);
}

bool hu_error_is_vision_unsupported_text(const char *text, size_t text_len) {
    if (!text || text_len == 0)
        return false;
    return hu_str_contains_ci(text, text_len, "does not support image", 21) ||
           hu_str_contains_ci(text, text_len, "doesn't support image", 21) ||
           hu_str_contains_ci(text, text_len, "image input not supported", 26) ||
           hu_str_contains_ci(text, text_len, "no endpoints found that support image input", 45) ||
           hu_str_contains_ci(text, text_len, "vision not supported", 19) ||
           hu_str_contains_ci(text, text_len, "multimodal not supported", 25);
}
