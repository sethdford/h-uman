/*
 * hu_io_secure — secure file creation helpers.
 *
 * See include/human/core/io_secure.h for the rationale and contract.
 */

#include "human/core/io_secure.h"

#include <stdbool.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

/* Match the path-traversal guard from src/memory/minja_guard.c and the
 * gateway path validator. Reject any path that contains ".." or a
 * percent-encoded variant. Conservative — we accept the false-positive
 * cost of rejecting files literally named "..something" because no
 * h-uman code path writes such files. */
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
