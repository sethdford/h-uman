/* src/core/paths.c — canonical $HOME / state-dir / chat.db resolution.
 * Contract, rationale and the two documented-override bugs this closes are in
 * include/human/core/paths.h. */
#include "human/core/paths.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

/* Copy `s` into buf with snprintf's return contract: full length, NUL-terminated
 * even on truncation, and >= cap signals truncation. Mirrors the shape the 164
 * migrated call sites already guard against. */
static int copy_snprintf_shaped(char *buf, size_t cap, const char *s) {
    size_t len = strlen(s);
    if (cap > 0) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
    }
    return (int)len;
}

int hu_paths_home(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return -1;
#if defined(_WIN32)
    /* Same order as hu_platform_get_home_dir(), which every raw getenv("HOME")
     * site was bypassing: USERPROFILE, then HOMEDRIVE+HOMEPATH. */
    const char *prof = getenv("USERPROFILE");
    if (prof && prof[0])
        return copy_snprintf_shaped(buf, cap, prof);
    const char *drive = getenv("HOMEDRIVE");
    const char *path = getenv("HOMEPATH");
    if (drive && drive[0] && path && path[0])
        return snprintf(buf, cap, "%s%s", drive, path);
    buf[0] = '\0';
    return -1;
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        buf[0] = '\0';
        return -1;
    }
    return copy_snprintf_shaped(buf, cap, home);
#endif
}

/* Resolve the state directory into `dir`. Semantics lifted verbatim from the
 * one prior implementation that honored the override (doctor.c
 * resolve_state_dir): HU_STATE_DIR set AND non-empty wins outright; a
 * set-but-empty value must fall through rather than resolve to "/". */
static int state_dir(char *dir, size_t cap) {
    const char *override = getenv("HU_STATE_DIR");
    if (override && override[0])
        return copy_snprintf_shaped(dir, cap, override);
    char home[1024];
    int hn = hu_paths_home(home, sizeof(home));
    if (hn < 0 || (size_t)hn >= sizeof(home))
        return -1;
    return snprintf(dir, cap, "%s/.human", home);
}

/* state_dir() plus the optional pre-migration fallback ("." / "/tmp"). */
static int resolve_state_dir(char *dir, size_t cap, const char *fallback_home) {
    int dn = state_dir(dir, cap);
    if (dn < 0 && fallback_home && fallback_home[0])
        dn = snprintf(dir, cap, "%s/.human", fallback_home);
    return dn;
}

int hu_paths_state_dir(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return -1;
    int n = resolve_state_dir(buf, cap, NULL);
    if (n < 0)
        buf[0] = '\0';
    return n;
}

int hu_paths_state_mkdir(char *buf, size_t cap) {
    int n = hu_paths_state_dir(buf, cap);
    if (n < 0 || (size_t)n >= cap)
        return n < 0 ? n : -1;
    /* mkdir -p: a fresh HOME in a test fixture, or HU_STATE_DIR pointing into a
     * directory that does not exist yet, must not leave the state dir silently
     * absent. EEXIST is the normal case; permissions match ~/.human's 0700. */
    for (size_t i = 1; buf[i]; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
#if defined(_WIN32)
        (void)_mkdir(buf);
#else
        (void)mkdir(buf, 0700);
#endif
        buf[i] = '/';
    }
#if defined(_WIN32)
    (void)_mkdir(buf);
#else
    (void)mkdir(buf, 0700);
#endif
    return n;
}

int hu_paths_state_dir_or(char *buf, size_t cap, const char *fallback_home) {
    if (!buf || cap == 0)
        return -1;
    int n = resolve_state_dir(buf, cap, fallback_home);
    if (n < 0)
        buf[0] = '\0';
    return n;
}

/* Shared core. `fallback_home` is the pre-migration behavior every call site
 * used to carry inline ("." or "/tmp" when HOME was unset): when the state
 * dir cannot be resolved, resolve to <fallback_home>/.human instead of
 * failing. NULL keeps the strict contract. */
static int state_join_v(char *buf, size_t cap, const char *fallback_home, const char *relfmt,
                        va_list ap) {
    if (!buf || cap == 0)
        return -1;
    char dir[1024];
    int dn = resolve_state_dir(dir, sizeof(dir), fallback_home);
    if (dn < 0 || (size_t)dn >= sizeof(dir)) {
        buf[0] = '\0';
        return -1;
    }
    if (!relfmt || !relfmt[0])
        return copy_snprintf_shaped(buf, cap, dir);
    /* Format the relative part on its own first so a caller's "%.256s" and
     * friends behave exactly as they did inline, then join. */
    char rel[1024];
    int rn = vsnprintf(rel, sizeof(rel), relfmt, ap);
    if (rn < 0 || (size_t)rn >= sizeof(rel)) {
        buf[0] = '\0';
        return -1;
    }
    return snprintf(buf, cap, "%s/%s", dir, rel);
}

int hu_paths_state(char *buf, size_t cap, const char *relfmt, ...) {
    va_list ap;
    va_start(ap, relfmt);
    int n = state_join_v(buf, cap, NULL, relfmt, ap);
    va_end(ap);
    return n;
}

int hu_paths_state_or(char *buf, size_t cap, const char *fallback_home, const char *relfmt, ...) {
    va_list ap;
    va_start(ap, relfmt);
    int n = state_join_v(buf, cap, fallback_home, relfmt, ap);
    va_end(ap);
    return n;
}

int hu_paths_chatdb(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return -1;
    /* The order config.h:294 documents: HU_CHATDB, then the macOS default. */
    const char *override = getenv("HU_CHATDB");
    if (override && override[0])
        return copy_snprintf_shaped(buf, cap, override);
    char home[1024];
    int hn = hu_paths_home(home, sizeof(home));
    if (hn < 0 || (size_t)hn >= sizeof(home)) {
        buf[0] = '\0';
        return -1;
    }
    return snprintf(buf, cap, "%s/Library/Messages/chat.db", home);
}

int hu_paths_chatdb_or(char *buf, size_t cap, const char *fallback_home) {
    int n = hu_paths_chatdb(buf, cap);
    if (n < 0 && buf && cap && fallback_home && fallback_home[0])
        n = snprintf(buf, cap, "%s/Library/Messages/chat.db", fallback_home);
    return n;
}
