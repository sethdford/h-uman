#ifndef HU_CORE_IO_SECURE_H
#define HU_CORE_IO_SECURE_H

/*
 * hu_io_secure — secure file creation helpers.
 *
 * CodeQL flags two recurring patterns across the codebase:
 *
 *   1. cpp/world-writable-file-creation
 *      fopen(path, "w") leaves the file mode to umask. With the typical
 *      umask 022 that means 0644, but no guarantee; a misconfigured
 *      umask 000 service writes 0666 / 0777 files.
 *
 *   2. cpp/path-injection
 *      Paths built from $HOME, $XDG_*, $HUMAN_* feed open()/unlink()
 *      without validating that the result doesn't escape its intended
 *      root via `..` or percent-encoded traversal.
 *
 * Both collapse to one fix: open() with an explicit mode and a
 * traversal guard, then fdopen() to keep the FILE* ergonomics. Use
 * this helper instead of fopen(path, "w") / fopen(path, "wb")
 * everywhere we create files derived from user, env, or config input.
 *
 * For sites that just need an existence check or read access, plain
 * fopen("r") / open(..., O_RDONLY) is fine — read-only opens don't
 * create world-writable artifacts.
 */

#include "human/core/error.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Permission class. Maps to a concrete POSIX mode bitmask in the
 * implementation; this enum is what callers reason about. */
typedef enum {
    /* 0600 — secrets, tokens, vault, OAuth, audit signing keys.
     * Nothing but the owning user can read or write. */
    HU_IO_PERM_SECRET = 0,
    /* 0644 — user data (config, persona, audio temp, session,
     * skill scaffold). Owner writes, world reads. */
    HU_IO_PERM_USER = 1,
} hu_io_perm_t;

/**
 * Open a file for writing with an explicit permission mode and a
 * path-traversal guard.
 *
 * Always creates (O_CREAT) and truncates (O_TRUNC) the target. Rejects
 * paths that contain ".." segments or percent-encoded traversal
 * sequences (`%2e`, `%2E`), matching the gateway path validator
 * (hu_minja_open_quarantine_fd, etc.).
 *
 * On non-POSIX platforms this falls back to fopen() with the requested
 * text/binary mode — the security guarantees only matter on POSIX where
 * file modes and traversal semantics are well-defined.
 *
 * @param path  filesystem path to create or truncate (must be non-NULL)
 * @param perm  HU_IO_PERM_SECRET (0600) or HU_IO_PERM_USER (0644)
 * @param mode  fopen mode string: "w" for text, "wb" for binary; only
 *              these two are accepted (anything else returns
 *              HU_ERR_INVALID_ARGUMENT)
 * @param out   receives the FILE* on success; set to NULL on failure
 * @return HU_OK on success; HU_ERR_INVALID_ARGUMENT for NULL inputs,
 *         unrecognized mode, or traversal in path; HU_ERR_IO for any
 *         filesystem error (errno preserved for the caller).
 */
hu_error_t hu_io_secure_open(const char *path, hu_io_perm_t perm,
                              const char *mode, FILE **out);

#ifdef __cplusplus
}
#endif

#endif /* HU_CORE_IO_SECURE_H */
