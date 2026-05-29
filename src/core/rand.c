/* src/core/rand.c — portable non-cryptographic uniform random.
 *
 * See include/human/core/rand.h for the contract. The platform split
 * mirrors the entropy sourcing proven in src/security/keystore.c:
 * arc4random on macOS and the BSDs (musl does not implement it), and
 * getrandom()/urandom on Linux with a non-blocking degraded fallback.
 */

#include "human/core/rand.h"

#include <stdlib.h> /* arc4random_uniform (BSD/macOS), rand/srand (fallback) */

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif

/* Fill `len` bytes from the OS CSPRNG. Returns 0 on success, -1 on failure.
 * Tries getrandom() first (no FD pressure, chroot-immune), then
 * /dev/urandom. Never blocks long-term: a failure is reported so the
 * caller can degrade rather than hang. */
static int rand_os_bytes(unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
#if defined(SYS_getrandom)
        long n = syscall(SYS_getrandom, buf + off, len - off, 0);
#else
        long n = -1;
        errno = ENOSYS;
#endif
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break; /* fall through to /dev/urandom */
        }
        off += (size_t)n;
    }
    if (off == len)
        return 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static uint32_t rand_u32(void) {
    uint32_t v;
    if (rand_os_bytes((unsigned char *)&v, sizeof(v)) == 0)
        return v;
    /* Degraded fallback: OS entropy briefly unavailable. Acceptable for
     * jitter/selection (never secrets). Seed once from time + stack address
     * so repeated calls in a process don't return a constant. */
    static int seeded = 0;
    if (!seeded) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        srand((unsigned int)(ts.tv_nsec ^ ts.tv_sec ^ (long)(intptr_t)&ts));
        seeded = 1;
    }
    /* rand() yields at least 15 bits; stitch 32 bits together. */
    return ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 2) ^ (uint32_t)rand();
}
#endif /* __linux__ */

uint32_t hu_rand_uniform(uint32_t bound) {
    if (bound < 2)
        return 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__)
    return arc4random_uniform(bound);
#elif defined(__linux__)
    /* Rejection sampling to eliminate modulo bias: discard draws in the
     * unrepresentable tail [limit, 2^32). */
    uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
    uint32_t r;
    do {
        r = rand_u32();
    } while (r >= limit);
    return r % bound;
#else
    /* Other POSIX: read entropy via stdio urandom; degrade to rand(). */
    uint32_t r;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(&r, 1, sizeof(r), f);
        fclose(f);
        if (got == sizeof(r)) {
            uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
            if (r < limit)
                return r % bound;
        }
    }
    return (uint32_t)rand() % bound;
#endif
}
