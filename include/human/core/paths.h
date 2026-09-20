#ifndef HU_CORE_PATHS_H
#define HU_CORE_PATHS_H

#include <stddef.h>

/* Canonical resolution of the three paths every subsystem builds by hand.
 *
 * Before 2026-07-27, `getenv("HOME")` followed by a hand-rolled snprintf
 * appeared 188 times across 88 files. That is not merely duplication — it is
 * where two DOCUMENTED overrides silently stopped working:
 *
 *   HU_CHATDB    documented in config.h:294 as the chat.db override, honored by
 *                4 of the 17 files that build that path. The other 13 — the
 *                iMessage channel itself, doctor, the calibration analyzers,
 *                persona auto-profile — went straight to $HOME and could not be
 *                pointed at a fixture.
 *   HU_STATE_DIR documented in doctor.h:198 as the ~/.human override, honored
 *                by exactly 1 of the 62 files that build paths under it
 *                (doctor.c). So doctor would inspect one state dir while the
 *                daemon, memory, persona and skills layers wrote to another.
 *
 * And every one of the 188 bypassed hu_platform_get_home_dir()'s Windows
 * handling (USERPROFILE / HOMEDRIVE+HOMEPATH), so only the 5 sites using that
 * accessor could ever resolve a home directory off POSIX.
 *
 * DROP-IN CONTRACT. Each function returns exactly what the snprintf it replaces
 * returned: bytes that would have been written (excluding the NUL), or a value
 * < 0 when the underlying environment is unset. A return >= cap means
 * truncation, as with snprintf. This is deliberate: the 164 call sites already
 * carry `int n = snprintf(...); if (n < 0 || (size_t)n >= sizeof(buf))` guards,
 * and preserving the return shape lets the migration be a line substitution
 * that leaves every guard intact and verifiable by the compiler. */

/* $HOME (POSIX) or USERPROFILE / HOMEDRIVE+HOMEPATH (Windows), non-allocating.
 * Returns -1 if no home directory can be resolved. */
int hu_paths_home(char *buf, size_t cap);

/* Failure contract for every function below: a negative return means the path
 * could not be resolved and, when cap > 0, buf holds "". Callers may treat
 * buf[0] == '\0' as "unresolved" without inspecting the count. */

/* The state directory: $HU_STATE_DIR if set and non-empty, else $HOME/.human.
 * `relfmt` is an optional printf-style path RELATIVE to that directory
 * ("memory.db", "skills/%.256s"); pass NULL for the directory itself. Never
 * inserts a second separator, so callers must not begin relfmt with '/'.
 * On any failure (unresolvable dir, truncation of the relative part) the
 * result is -1 and buf[0] == '\0' whenever cap > 0, so callers may test the
 * buffer or the return value interchangeably. */
int hu_paths_state(char *buf, size_t cap, const char *relfmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* The iMessage database: $HU_CHATDB if set and non-empty, else
 * $HOME/Library/Messages/chat.db. This is the order config.h documents. */
int hu_paths_chatdb(char *buf, size_t cap);

/* Fallback variants. Identical to the strict forms except that when the state
 * dir / chat.db cannot be resolved (HOME unset and no override), the result is
 * <fallback_home>/.human/<rel> or <fallback_home>/Library/Messages/chat.db
 * instead of -1. This is the pre-migration behavior those call sites carried
 * inline as `if (!home) home = "."` — kept explicit here so the semantics are
 * visible at the call and testable in one place. A NULL/empty fallback_home
 * degrades to the strict contract. Return value is snprintf-shaped. */
int hu_paths_state_or(char *buf, size_t cap, const char *fallback_home, const char *relfmt, ...)
    __attribute__((format(printf, 4, 5)));
int hu_paths_chatdb_or(char *buf, size_t cap, const char *fallback_home);

/* The bare state directory ($HU_STATE_DIR, else $HOME/.human) — use these
 * rather than passing a NULL format to the printf-attributed forms. */
int hu_paths_state_dir(char *buf, size_t cap);
int hu_paths_state_dir_or(char *buf, size_t cap, const char *fallback_home);

/* Resolve the state dir AND create it (0700, parents included) if missing. Returns like
 * hu_paths_state_dir; the mkdir result is deliberately ignored (EEXIST is the
 * normal case) — callers that need to know the dir exists check access(). */
int hu_paths_state_mkdir(char *buf, size_t cap);

#endif /* HU_CORE_PATHS_H */
