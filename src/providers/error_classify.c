#include "seaclaw/providers/error_classify.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool contains_fold(const char *haystack, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0) return true;
    if (hlen < nlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen && match; j++)
            match = (to_lower((unsigned char)haystack[i + j]) == to_lower((unsigned char)needle[j]));
        if (match) return true;
    }
    return false;
}

bool sc_error_is_non_retryable(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0) return false;
    for (size_t i = 0; i < msg_len; ) {
        if (!isdigit((unsigned char)msg[i])) { i++; continue; }
        size_t end = i;
        while (end < msg_len && isdigit((unsigned char)msg[end])) end++;
        if (end - i == 3) {
            char buf[4];
            buf[0] = msg[i]; buf[1] = msg[i+1]; buf[2] = msg[i+2]; buf[3] = '\0';
            int code = atoi(buf);
            if (code >= 400 && code < 500 && code != 429 && code != 408)
                return true;
        }
        i = end;
    }
    return false;
}

bool sc_error_is_context_exhausted(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0) return false;
    size_t check_len = msg_len > 512 ? 512 : msg_len;
    bool has_context = contains_fold(msg, check_len, "context", 7);
    bool has_token = contains_fold(msg, check_len, "token", 5);
    if (has_context && (contains_fold(msg, check_len, "length", 6) ||
            contains_fold(msg, check_len, "maximum", 7) ||
            contains_fold(msg, check_len, "window", 6) ||
            contains_fold(msg, check_len, "exceed", 6)))
        return true;
    if (has_token && (contains_fold(msg, check_len, "limit", 5) ||
            contains_fold(msg, check_len, "too many", 8) ||
            contains_fold(msg, check_len, "maximum", 7) ||
            contains_fold(msg, check_len, "exceed", 6)))
        return true;
    if (contains_fold(msg, check_len, "413", 3) && contains_fold(msg, check_len, "too large", 9))
        return true;
    return false;
}

bool sc_error_is_rate_limited(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0) return false;
    size_t check_len = msg_len > 512 ? 512 : msg_len;
    if (contains_fold(msg, check_len, "ratelimited", 11) ||
            contains_fold(msg, check_len, "rate limited", 12) ||
            contains_fold(msg, check_len, "rate_limit", 10) ||
            contains_fold(msg, check_len, "too many requests", 17) ||
            contains_fold(msg, check_len, "quota exceeded", 14) ||
            contains_fold(msg, check_len, "throttle", 8))
        return true;
    return contains_fold(msg, check_len, "429", 3) &&
        (contains_fold(msg, check_len, "rate", 4) ||
         contains_fold(msg, check_len, "limit", 5) ||
         contains_fold(msg, check_len, "too many", 8));
}

uint64_t sc_error_parse_retry_after_ms(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0) return 0;
    static const char *prefixes[] = { "retry-after:", "retry_after:", "retry-after ", "retry_after " };
    size_t check_len = msg_len > 4096 ? 4096 : msg_len;

    for (size_t p = 0; p < sizeof(prefixes)/sizeof(prefixes[0]); p++) {
        size_t plen = strlen(prefixes[p]);
        for (size_t i = 0; i + plen <= check_len; i++) {
            if (strncasecmp(msg + i, prefixes[p], plen) != 0) continue;
            size_t start = i + plen;
            while (start < check_len && (msg[start] == ' ' || msg[start] == '\t')) start++;
            size_t end = start;
            while (end < check_len && (isdigit((unsigned char)msg[end]) || msg[end] == '.')) end++;
            if (end > start) {
                char buf[32];
                size_t copy = end - start < 31 ? end - start : 31;
                memcpy(buf, msg + start, copy);
                buf[copy] = '\0';
                double secs = atof(buf);
                if (secs >= 0.0) return (uint64_t)(secs * 1000.0);
            }
        }
    }
    return 0;
}

bool sc_error_is_rate_limited_text(const char *text, size_t text_len) {
    return sc_error_is_rate_limited(text, text_len);
}

bool sc_error_is_context_exhausted_text(const char *text, size_t text_len) {
    return sc_error_is_context_exhausted(text, text_len);
}

bool sc_error_is_vision_unsupported_text(const char *text, size_t text_len) {
    if (!text || text_len == 0) return false;
    return contains_fold(text, text_len, "does not support image", 21) ||
           contains_fold(text, text_len, "doesn't support image", 21) ||
           contains_fold(text, text_len, "image input not supported", 26) ||
           contains_fold(text, text_len, "no endpoints found that support image input", 45) ||
           contains_fold(text, text_len, "vision not supported", 19) ||
           contains_fold(text, text_len, "multimodal not supported", 25);
}
