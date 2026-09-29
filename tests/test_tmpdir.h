/*
 * tests/test_tmpdir.h — collision-proof temp dirs + recursive cleanup for tests.
 *
 * Several test helpers historically named temp dirs `/tmp/hu_*_<pid>_<tag>`.
 * Because PIDs recycle across runs and the dirs were never removed, a later
 * run could `mkdir` onto a leftover dir (EEXIST) or inherit poisoned state
 * (e.g. a keystore tombstone, stale gate files) — producing spurious local
 * failures that CI never sees (CI gets an ephemeral clean /tmp).
 *
 * Use hu_test_mkdtemp() for a unique dir every run (no collision possible),
 * and hu_test_rm_rf() to clean it up so nothing leaks. No process spawning
 * (tests must not shell out) — this is a pure libc recursive remove.
 *
 * Header-only static inline so it adds no link symbols and never warns when
 * a translation unit includes it without using every helper.
 */
#ifndef HU_TEST_TMPDIR_H
#define HU_TEST_TMPDIR_H

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Recursively remove a file or directory tree. Best-effort: ignores errors
 * (a test cleaning up should never fail the test). Safe on a NULL/empty path
 * and on a path that does not exist. */
static inline void hu_test_rm_rf(const char *path) {
    if (!path || !*path)
        return;
    struct stat st;
    if (lstat(path, &st) != 0)
        return; /* nothing there */
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                char child[1024];
                int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                if (n > 0 && (size_t)n < sizeof(child))
                    hu_test_rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* Create a unique temp directory: `prefix` + "XXXXXX", resolved by mkdtemp.
 * Writes the resulting path into `out` (capacity `out_sz`). Returns true on
 * success. The random suffix makes cross-run collisions impossible, so callers
 * never hit EEXIST regardless of PID reuse or leftover dirs. Pair with
 * hu_test_rm_rf(out) when the test is done. */
static inline bool hu_test_mkdtemp(const char *prefix, char *out, size_t out_sz) {
    int n = snprintf(out, out_sz, "%sXXXXXX", prefix ? prefix : "/tmp/hu_test_");
    if (n <= 0 || (size_t)n >= out_sz)
        return false;
    return mkdtemp(out) != NULL;
}

/* Per-process unique temp directory: "<$TMPDIR>/hu_<tag>_<pid>_XXXXXX"
 * ($TMPDIR falls back to /tmp). Use this instead of a fixed "/tmp/hu_foo"
 * literal: two suites running concurrently (e.g. two sessions' pre-push
 * hooks) would otherwise write and delete the SAME file underneath each
 * other's assertions. Writes the created path into `buf`; returns true on
 * success. Clean up with hu_test_rm_rf(buf). */
static inline bool hu_test_tmpdir(char *buf, size_t cap, const char *tag) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base)
        base = "/tmp";
    size_t blen = strlen(base);
    while (blen > 1 && base[blen - 1] == '/')
        blen--; /* $TMPDIR on macOS ends in "/" — avoid "T//hu_..." */
    int n = snprintf(buf, cap, "%.*s/hu_%s_%ld_XXXXXX", (int)blen, base, tag ? tag : "test",
                     (long)getpid());
    if (n <= 0 || (size_t)n >= cap)
        return false;
    return mkdtemp(buf) != NULL;
}

/* Convenience for the common "one scratch FILE per test" shape: returns
 * "<root>/<name>" in `buf`, where <root> is a per-process hu_test_tmpdir()
 * created lazily (one per including translation unit) and removed
 * recursively at exit. Tests keep their existing unlink()/remove() of the
 * file; the root sweeps up anything they forget. Returns true on success. */
static char hu_test__tmproot[512];
static inline void hu_test__tmproot_cleanup(void) {
    hu_test_rm_rf(hu_test__tmproot);
}
static inline bool hu_test_tmppath(char *buf, size_t cap, const char *name) {
    if (!hu_test__tmproot[0]) {
        if (!hu_test_tmpdir(hu_test__tmproot, sizeof(hu_test__tmproot), "tests"))
            return false;
        atexit(hu_test__tmproot_cleanup);
    }
    int n = snprintf(buf, cap, "%s/%s", hu_test__tmproot, name ? name : "scratch");
    return n > 0 && (size_t)n < cap;
}

#endif /* HU_TEST_TMPDIR_H */
