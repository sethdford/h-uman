/*
 * hu_io_secure — secure file creation helpers.
 *
 * See include/human/core/io_secure.h for the rationale and contract.
 */

#include "human/core/io_secure.h"

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <stdio.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Match the path-traversal guard from src/memory/minja_guard.c and the
 * gateway path validator. Reject any path that contains ".." or a
 * percent-encoded variant. Conservative — we accept the false-positive
 * cost of rejecting files literally named "..something" because no
 * h-uman code path writes such files. */
#define HU_IO_ATOMIC_PATH_MAX 4096

static bool path_has_traversal(const char *path) {
    return path == NULL || strstr(path, "..") != NULL || strstr(path, "%2e") != NULL ||
           strstr(path, "%2E") != NULL;
}

/* Translate the enum to a concrete POSIX mode. Centralised so future
 * tuning (e.g. group-readable variants for shared install paths) lives
 * in one spot. */
#ifndef _WIN32
static int posix_mode_for(hu_io_perm_t perm) {
    switch (perm) {
    case HU_IO_PERM_SECRET:
        return 0600;
    case HU_IO_PERM_USER:
        return 0644;
    }
    /* Unreachable under -Wswitch-enum but the compiler doesn't know
     * that — return the more restrictive choice on any unknown value
     * so we fail closed. */
    return 0600;
}
#endif

hu_error_t hu_io_secure_open(const char *path, hu_io_perm_t perm, const char *mode, FILE **out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || !mode)
        return HU_ERR_INVALID_ARGUMENT;
    /* We only support the two write modes that the callers actually
     * need. Anything else is almost certainly a bug — `"a"` (append)
     * should be a separate API since appending and creating have
     * different security stories. */
    if (strcmp(mode, "w") != 0 && strcmp(mode, "wb") != 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (path_has_traversal(path))
        return HU_ERR_INVALID_ARGUMENT;

#ifdef _WIN32
    /* Windows file modes don't map cleanly to POSIX permissions and
     * the world-writable threat model is different. Fall back to plain
     * fopen — the security guarantees only matter on POSIX. */
    (void)perm;
    FILE *f = fopen(path, mode);
    if (!f)
        return HU_ERR_IO;
    *out = f;
    return HU_OK;
#else
    int mode_bits = posix_mode_for(perm);
    /* O_TRUNC matches the fopen("w") / fopen("wb") semantics that the
     * callers expect. O_NOFOLLOW would be safer but breaks legitimate
     * config-symlink workflows; leave it for a follow-up tuning pass. */
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode_bits);
    if (fd < 0)
        return HU_ERR_IO;
    FILE *f = fdopen(fd, mode);
    if (!f) {
        close(fd);
        return HU_ERR_IO;
    }
    *out = f;
    return HU_OK;
#endif
}

hu_error_t hu_io_secure_write_atomic(const char *path, hu_io_perm_t perm, const void *data,
                                     size_t len) {
    if (!path || (!data && len > 0) || path_has_traversal(path))
        return HU_ERR_INVALID_ARGUMENT;
    char tmp[HU_IO_ATOMIC_PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

#ifdef _WIN32
    /* No mkstemp / fsync / atomic replace-rename on this fallback path.
     * Still never truncate the target in place: write the temp copy fully,
     * then swap it in. */
    (void)perm;
    memcpy(tmp + n - 6, "win000", 6);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return HU_ERR_IO;
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || (remove(path) != 0 && errno != ENOENT) || rename(tmp, path) != 0) {
        (void)remove(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
#else
    const unsigned char *p = (const unsigned char *)data;
    size_t left = len;
    int fd = mkstemp(tmp);
    if (fd < 0)
        return HU_ERR_IO;
    /* mkstemp creates 0600; widen only when the caller asked for 0644. */
    if (fchmod(fd, (mode_t)posix_mode_for(perm)) != 0)
        goto fail;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            goto fail;
        p += w;
        left -= (size_t)w;
    }
    /* fsync before rename: otherwise a crash can leave the NEW name
     * pointing at an empty file on filesystems that reorder metadata. */
    if (fsync(fd) != 0)
        goto fail;
    int closed = close(fd);
    fd = -1;
    if (closed != 0 || rename(tmp, path) != 0)
        goto fail;
    hu_io_secure_sync_parent_dir(path);
    return HU_OK;

fail:
    if (fd >= 0)
        close(fd);
    (void)unlink(tmp);
    return HU_ERR_IO;
#endif
}

void hu_io_secure_sync_parent_dir(const char *path) {
#ifdef _WIN32
    (void)path;
#else
    char dir[HU_IO_ATOMIC_PATH_MAX];
    size_t n = path ? strlen(path) : sizeof(dir);
    if (n >= sizeof(dir))
        return;
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    if (slash == dir)
        slash[1] = '\0';
    else
        *slash = '\0';
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)fsync(fd);
    close(fd);
#endif
}
