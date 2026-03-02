#ifndef SC_PLATFORM_H
#define SC_PLATFORM_H

#include "core/allocator.h"
#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Environment and paths
 * ────────────────────────────────────────────────────────────────────────── */

/* Get env var. Caller owns returned string (allocator.free). Returns NULL if
   not found or on OOM. */
char *sc_platform_get_env(sc_allocator_t *alloc, const char *name);

/* Get user home directory. Caller owns returned string. Returns NULL on error. */
char *sc_platform_get_home_dir(sc_allocator_t *alloc);

/* Get system temp directory. Caller owns returned string. Returns NULL on error. */
char *sc_platform_get_temp_dir(sc_allocator_t *alloc);

/* ──────────────────────────────────────────────────────────────────────────
 * Shell
 * ────────────────────────────────────────────────────────────────────────── */

/* Platform shell for executing commands (e.g. /bin/sh, cmd.exe). */
const char *sc_platform_get_shell(void);

/* Shell flag for passing a command string (e.g. -c, /c). */
const char *sc_platform_get_shell_flag(void);

/* ──────────────────────────────────────────────────────────────────────────
 * Platform detection
 * ────────────────────────────────────────────────────────────────────────── */

bool sc_platform_is_windows(void);
bool sc_platform_is_unix(void);

#endif /* SC_PLATFORM_H */
