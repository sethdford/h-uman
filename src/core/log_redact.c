/* Keep message text and handles out of production logs. See log_redact.h. */
#include "human/core/log_redact.h"
#include "human/core/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_content_override = -1;

bool hu_log_content_enabled(void) {
    if (g_content_override >= 0)
        return g_content_override != 0;
    const char *v = getenv("HU_LOG_CONTENT");
    return v && v[0] == '1' && v[1] == '\0';
}

void hu_log_content_set_for_test(int on_or_minus1) {
    g_content_override = on_or_minus1;
}

/* ── SipHash-2-4 (Aumasson & Bernstein), 64-bit output ───────────────── */

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND           \
    do {                   \
        v0 += v1;          \
        v1 = ROTL(v1, 13); \
        v1 ^= v0;          \
        v0 = ROTL(v0, 32); \
        v2 += v3;          \
        v3 = ROTL(v3, 16); \
        v3 ^= v2;          \
        v0 += v3;          \
        v3 = ROTL(v3, 21); \
        v3 ^= v0;          \
        v2 += v1;          \
        v1 = ROTL(v1, 17); \
        v1 ^= v2;          \
        v2 = ROTL(v2, 32); \
    } while (0)

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint64_t siphash24(const uint8_t key[16], const uint8_t *in, size_t len) {
    uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    size_t full = len - (len % 8);
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m = le64(in + i);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < len % 8; i++)
        b |= (uint64_t)in[full + i] << (8 * i);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

/* ── The per-install key ─────────────────────────────────────────────── */

static int random_bytes(uint8_t *buf, size_t len) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, len);
    return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    return got == len ? 0 : -1;
#endif
}

bool hu_log_tag_key_load(const char *dir, uint8_t key[16]) {
    if (!dir || !key)
        return false;
    char path[1024];
    int pn = snprintf(path, sizeof(path), "%s/log_tag.key", dir);
    if (pn < 0 || (size_t)pn >= sizeof(path))
        return false;
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            uint8_t buf[17];
            ssize_t n = read(fd, buf, sizeof(buf));
            close(fd);
            if (n != 16)
                return false; /* short or oversized: never guess a key */
            memcpy(key, buf, 16);
            return true;
        }
        if (errno != ENOENT)
            return false;
        uint8_t fresh[16];
        if (random_bytes(fresh, sizeof(fresh)) != 0)
            return false;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue; /* another process won the race: read theirs */
            return false;
        }
        bool ok = write(fd, fresh, sizeof(fresh)) == (ssize_t)sizeof(fresh);
        ok = (fchmod(fd, 0600) == 0) && ok;
        close(fd);
        if (!ok) {
            (void)unlink(path);
            return false;
        }
        memcpy(key, fresh, 16);
        return true;
    }
    return false;
}

static uint8_t g_key[16];
static bool g_key_test_set;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

static void key_init(void) {
#ifdef HU_IS_TEST
    /* Tests never touch the state dir: a fixed, non-zero key. */
    for (int i = 0; i < 16; i++)
        g_key[i] = (uint8_t)(0xA5 ^ (i * 29));
#else
    char dir[1024];
    if (hu_paths_state_dir(dir, sizeof(dir)) > 0 && hu_log_tag_key_load(dir, g_key))
        return;
    /* No state dir: a per-process random key keeps tags unlinkable, at the
     * cost of stability across restarts. */
    if (random_bytes(g_key, sizeof(g_key)) != 0)
        for (int i = 0; i < 16; i++)
            g_key[i] = (uint8_t)((uintptr_t)&g_key >> (i % 8));
#endif
}

void hu_log_tag_set_key_for_test(const uint8_t *key16) {
    if (key16) {
        memcpy(g_key, key16, 16);
        g_key_test_set = true;
    } else {
        key_init(); /* the build default again */
        g_key_test_set = false;
    }
}

uint64_t hu_log_contact_tag(const char *handle, size_t len) {
    if (!g_key_test_set)
        (void)pthread_once(&g_key_once, key_init);
    return siphash24(g_key, (const uint8_t *)(handle ? handle : ""), handle ? len : 0) &
           0xFFFFFFFFFFFFULL;
}

const char *hu_log_who(const char *handle, size_t len, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return "";
    if (!handle || len == 0)
        snprintf(buf, cap, "-");
    else if (hu_log_content_enabled())
        snprintf(buf, cap, "%.*s", (int)(len > 24 ? 24 : len), handle);
    else
        snprintf(buf, cap, "#%012llx", (unsigned long long)hu_log_contact_tag(handle, len));
    return buf;
}

const char *hu_log_text(const char *text, size_t len, size_t max, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return "";
    if (!text)
        len = 0;
    if (hu_log_content_enabled())
        snprintf(buf, cap, "%.*s", (int)(len > max ? max : len), text ? text : "");
    else
        snprintf(buf, cap, "<%zu chars>", len);
    return buf;
}
