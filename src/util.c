#include "seaclaw/util.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

size_t sc_util_trim(char *s, size_t len) {
    if (!s || len == 0) return 0;
    size_t start = 0;
    while (start < len && (unsigned char)s[start] <= ' ') start++;
    size_t end = len;
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    if (start > 0) {
        memmove(s, s + start, end - start);
    }
    end -= start;
    if (end < len) s[end] = '\0';
    return end;
}

char *sc_util_strdup(void *ctx, void *(*alloc)(void *, size_t), const char *s) {
    if (!ctx || !alloc || !s) return NULL;
    size_t len = strlen(s) + 1;
    char *out = alloc(ctx, len);
    if (out) memcpy(out, s, len);
    return out;
}

void sc_util_strfree(void *ctx, void (*free_fn)(void *, void *, size_t),
                     char *s) {
    if (!ctx || !free_fn || !s) return;
    free_fn(ctx, s, strlen(s) + 1);
}

int sc_util_strcasecmp(const char *a, const char *b) {
    if (!a) return b ? -1 : 0;
    if (!b) return 1;
    for (;;) {
        int ca = (unsigned char)tolower((unsigned char)*a++);
        int cb = (unsigned char)tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

static void fill_random_bytes(unsigned char *buf, size_t n) {
#ifdef _WIN32
    (void)buf;
    (void)n;
    /* Fallback: use time + pid for minimal uniqueness in tests */
    for (size_t i = 0; i < n; i++) {
        buf[i] = (unsigned char)(rand() & 0xff);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t done = 0;
        while (done < n) {
            ssize_t r = read(fd, buf + done, n - done);
            if (r <= 0) break;
            done += (size_t)r;
        }
        close(fd);
        if (done >= n) return;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = (unsigned char)(rand() & 0xff);
    }
#endif
}

char *sc_util_gen_session_id(void *ctx, void *(*alloc)(void *, size_t)) {
    if (!alloc) return NULL;
    unsigned char r[8];
    fill_random_bytes(r, sizeof(r));
    char buf[32];
    time_t t = time(NULL);
    int n = snprintf(buf, sizeof(buf), "%lx%02x%02x%02x%02x",
                     (unsigned long)t, r[0], r[1], r[2], r[3]);
    if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
    size_t len = (size_t)n + 1;
    char *out = alloc(ctx, len);
    if (out) {
        memcpy(out, buf, (size_t)n + 1);
    }
    return out;
}
