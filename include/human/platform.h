#ifndef HU_PLATFORM_H
#define HU_PLATFORM_H

#include "core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Environment and paths
 * ────────────────────────────────────────────────────────────────────────── */

/* Get env var. Caller owns returned string (allocator.free). Returns NULL if
   not found or on OOM. */
char *hu_platform_get_env(hu_allocator_t *alloc, const char *name);

/* Get user home directory. Caller owns returned string. Returns NULL on error. */
char *hu_platform_get_home_dir(hu_allocator_t *alloc);

/* Get system temp directory. Caller owns returned string. Returns NULL on error. */
char *hu_platform_get_temp_dir(hu_allocator_t *alloc);

/* ──────────────────────────────────────────────────────────────────────────
 * Shell
 * ────────────────────────────────────────────────────────────────────────── */

/* Platform shell for executing commands (e.g. /bin/sh, cmd.exe). */
const char *hu_platform_get_shell(void);

/* Shell flag for passing a command string (e.g. -c, /c). */
const char *hu_platform_get_shell_flag(void);

/* ──────────────────────────────────────────────────────────────────────────
 * Platform detection
 * ────────────────────────────────────────────────────────────────────────── */

bool hu_platform_is_windows(void);
bool hu_platform_is_unix(void);

/* ──────────────────────────────────────────────────────────────────────────
 * Cross-platform time, sleep, filesystem (thread-safe, Windows-compatible)
 * ────────────────────────────────────────────────────────────────────────── */

#include <time.h>

/* Thread-safe localtime. Fills *out and returns out on success, NULL on failure. */
struct tm *hu_platform_localtime_r(const time_t *t, struct tm *out);

/* Thread-safe gmtime. Fills *out and returns out on success, NULL on failure. */
struct tm *hu_platform_gmtime_r(const time_t *t, struct tm *out);

/* Sleep for ms milliseconds. */
void hu_platform_sleep_ms(unsigned int ms);

/* Create directory. mode ignored on Windows. Returns 0 on success, -1 on error. */
int hu_platform_mkdir(const char *path, unsigned int mode);

/* Resolve path to absolute. Caller frees result. Returns NULL on failure. */
char *hu_platform_realpath(hu_allocator_t *alloc, const char *path);

/* Parse "YYYY-MM-DD HH:MM" or "HH:MM" into tm. Returns true on success. */
bool hu_platform_parse_datetime(const char *ts, struct tm *out);

/* Get home env var (HOME or USERPROFILE). Returns non-owned pointer or NULL. */
const char *hu_platform_get_home_env(void);

#endif /* HU_PLATFORM_H */
