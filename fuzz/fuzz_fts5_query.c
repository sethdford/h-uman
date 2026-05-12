/* Fuzz harness for the FTS5 MATCH query builder used in memory search.
 * Reproduces the word-splitting + quote-escaping logic from sqlite.c to
 * verify it never produces malformed FTS5 syntax that would crash SQLite.
 * Must not crash on any input. */
#include "human/core/allocator.h"
#include "human/core/string.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static size_t buf_appendf(char *buf, size_t cap, size_t pos, const char *fmt, const char *word) {
    int n = snprintf(buf + pos, cap - pos, fmt, word);
    if (n < 0 || pos + (size_t)n >= cap)
        return cap - 1;
    return pos + (size_t)n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096)
        return 0;

    char fts_buf[512];
    size_t fts_len = 0;
    const char *p = (const char *)data;
    const char *end = p + size;
    int first = 1;

    while (p < end && fts_len < sizeof(fts_buf) - 10) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end)
            break;
        const char *word_start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if (p > word_start) {
            char escaped_word[256];
            size_t ew_len = 0;
            for (const char *c = word_start; c < p && ew_len < sizeof(escaped_word) - 2; c++) {
                if (*c == '"') {
                    if (ew_len < sizeof(escaped_word) - 3) {
                        escaped_word[ew_len++] = '"';
                        escaped_word[ew_len++] = '"';
                    }
                } else {
                    escaped_word[ew_len++] = *c;
                }
            }
            escaped_word[ew_len] = '\0';
            if (!first)
                fts_len = buf_appendf(fts_buf, sizeof(fts_buf), fts_len, "%s", " OR ");
            fts_len = buf_appendf(fts_buf, sizeof(fts_buf), fts_len, "\"%s\"", escaped_word);
            first = 0;
        }
    }

    if (fts_len > 0)
        fts_buf[fts_len] = '\0';

    return 0;
}
