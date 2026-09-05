/* src/util/llm_json.c — see include/human/util/llm_json.h */
#include "human/util/llm_json.h"

#include <ctype.h>
#include <string.h>

/* Case-insensitive search for `needle` (NUL-terminated) in s[0..len). */
static const char *find_ci(const char *s, size_t len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen)
        return NULL;
    for (size_t i = 0; i + nlen <= len; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)s[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen)
            return s + i;
    }
    return NULL;
}

/* Last occurrence of a closing reasoning tag; NULL when none. */
static const char *last_close_tag(const char *s, size_t len, size_t *tag_len) {
    static const char *const tags[] = {"</think>", "</thought>"};
    const char *best = NULL;
    size_t best_len = 0;
    for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); t++) {
        size_t off = 0;
        for (;;) {
            const char *hit = find_ci(s + off, len - off, tags[t]);
            if (!hit)
                break;
            if (!best || hit > best) {
                best = hit;
                best_len = strlen(tags[t]);
            }
            off = (size_t)(hit - s) + 1;
            if (off >= len)
                break;
        }
    }
    *tag_len = best_len;
    return best;
}

static bool has_open_tag(const char *s, size_t len) {
    return find_ci(s, len, "<think>") != NULL || find_ci(s, len, "<thought>") != NULL;
}

bool hu_llm_json_locate(const char *s, size_t len, const char **out, size_t *out_len) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!s || len == 0 || !out || !out_len)
        return false;

    /* 1. Skip reasoning. Everything before the last close tag is thought,
     *    not answer. An open tag with no close is a truncated thought. */
    size_t tag_len = 0;
    const char *close = last_close_tag(s, len, &tag_len);
    if (close) {
        size_t skip = (size_t)(close - s) + tag_len;
        s += skip;
        len -= skip;
    } else if (has_open_tag(s, len)) {
        return false;
    }

    /* 2. First bracket starts the candidate. */
    size_t start = 0;
    while (start < len && s[start] != '{' && s[start] != '[')
        start++;
    if (start >= len)
        return false;

    /* 3. String-aware balanced scan to the matching close. */
    int depth = 0;
    bool in_str = false;
    for (size_t i = start; i < len; i++) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') {
                i++; /* skip the escaped byte */
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) {
                *out = s + start;
                *out_len = i - start + 1;
                return true;
            }
            if (depth < 0)
                return false;
        }
    }
    return false; /* unbalanced: truncated */
}
